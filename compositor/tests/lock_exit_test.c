// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the actual fatal exit with startup's supervisor environment already
// removed. The preload barrier stops the child before it can signal this test.
#undef NDEBUG
#include "../src/lock.c"
#include <assert.h>

static void check_exit(bool supervised)
{
    char directory[] = "/tmp/vela-lock-exit-XXXXXX";
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

int main(void)
{
    check_exit(false);
    check_exit(true);
    puts("lock exit: ok");
    return 0;
}
