// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the actual fatal exit with startup's supervisor environment already
// removed. The preload barrier stops the child before it can signal this test.
#undef NDEBUG
#include "../src/lock.c"
#include <assert.h>
#include <pthread.h>
#include <sys/resource.h>

static void check_exit(bool supervised)
{
    // CTest runs in the build directory, not a shared system temp directory.
    // mkdtemp creates an unpredictable, mode-0700 directory for the trigger.
    char directory[] = "./vela-lock-exit-XXXXXX";
    assert(mkdtemp(directory));
    int fd = open(directory, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    int trigger = openat(fd, "lock-exit-stop", O_CREAT | O_WRONLY, 0600);
    assert(trigger >= 0);
    close(trigger);
    char number[32];
    snprintf(number, sizeof(number), "%d", fd);
    assert(setenv("VELA_TEST_FAIL_DIR", number, 1) == 0);
    assert(unsetenv("VELA_WAYLAND_SOCKET_FD") == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        alarm(5);
        struct vela_server server = { .supervised = supervised, .runtime_dir_fd = fd };
        struct vela_lock lock = { .server = &server };
        lock_unsafe_exit(&lock);
    }
    int status;
    assert(waitpid(child, &status, WUNTRACED) == child);
    if (supervised) {
        assert(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
        assert(faccessat(fd, "lock-exit-stop", F_OK, 0) < 0 && errno == ENOENT);
        assert(kill(child, SIGKILL) == 0);
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    } else {
        assert(WIFEXITED(status) && WEXITSTATUS(status) == VELA_EXIT_LOCK_UNSAFE);
        assert(unlinkat(fd, "lock-exit-stop", 0) == 0);
    }
    close(fd);
    assert(rmdir(directory) == 0);
    unsetenv("VELA_TEST_FAIL_DIR");
}

static volatile sig_atomic_t parent_stopped;

static void parent_stop(int signum)
{
    (void)signum;
    parent_stopped = 1;
}

static void *worker(void *data)
{
    pthread_barrier_t *ready = data;
    int result = pthread_barrier_wait(ready);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (;;) {
        pause();
    }
    return NULL;
}

static void check_crash_before_parent_signal(int crash)
{
    char directory[] = "./vela-lock-crash-XXXXXX";
    assert(mkdtemp(directory));
    int fd = open(directory, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    FILE *trigger = fdopen(openat(fd, "lock-exit-stop", O_CREAT | O_WRONLY, 0600), "w");
    assert(trigger);
    fprintf(trigger, "%d", crash);
    assert(fclose(trigger) == 0);
    char number[32];
    snprintf(number, sizeof(number), "%d", fd);
    assert(setenv("VELA_TEST_FAIL_DIR", number, 1) == 0);
    parent_stopped = 0;
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        // Model a library's altered signal disposition and inherited masks.
        // The injected crash must still terminate this process, with the exact
        // requested signal, before its main thread can signal the supervisor.
        struct rlimit no_core = { 0, 0 };
        assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
        alarm(5);
        struct sigaction ignored = { .sa_handler = SIG_IGN };
        sigemptyset(&ignored.sa_mask);
        assert(sigaction(crash, &ignored, NULL) == 0);
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, crash);
        assert(pthread_sigmask(SIG_BLOCK, &blocked, NULL) == 0);
        enum { WORKERS = 4 };
        pthread_barrier_t ready;
        assert(pthread_barrier_init(&ready, NULL, WORKERS + 1) == 0);
        pthread_t threads[WORKERS];
        for (int i = 0; i < WORKERS; ++i) {
            assert(pthread_create(&threads[i], NULL, worker, &ready) == 0);
        }
        int result = pthread_barrier_wait(&ready);
        assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
        struct vela_server server = { .supervised = true, .runtime_dir_fd = fd };
        struct vela_lock lock = { .server = &server };
        lock_unsafe_exit(&lock);
    }
    int status;
    assert(waitpid(child, &status, WUNTRACED) == child);
    assert(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
    assert(!parent_stopped);
    assert(faccessat(fd, "lock-exit-stop", F_OK, 0) < 0 && errno == ENOENT);
    // No process-directed crash signal: only the thread at the barrier may
    // inject it. Driver worker scheduling cannot run the real kill first.
    assert(kill(child, SIGCONT) == 0);
    pid_t reaped;
    do {
        reaped = waitpid(child, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    assert(reaped == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == crash);
    assert(!parent_stopped);
    close(fd);
    assert(rmdir(directory) == 0);
    unsetenv("VELA_TEST_FAIL_DIR");
}

int main(void)
{
    check_exit(false);
    check_exit(true);
    struct sigaction stop = { .sa_handler = parent_stop }, previous;
    sigemptyset(&stop.sa_mask);
    assert(sigaction(SIGTERM, &stop, &previous) == 0);
    for (int repeat = 0; repeat < 10; ++repeat) {
        check_crash_before_parent_signal(SIGSEGV);
        check_crash_before_parent_signal(SIGABRT);
    }
    assert(sigaction(SIGTERM, &previous, NULL) == 0);
    puts("lock exit: ok");
    return 0;
}
