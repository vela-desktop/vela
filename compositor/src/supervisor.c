// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The session supervisor (vela-compositor --supervise ...).
//
// If the compositor crashes, the session must not go down with it. The
// supervisor is a tiny process that touches neither the GPU nor wlroots: it
// creates the Wayland socket (wayland-N), keeps it open and starts the real
// compositor handing it the socket. If the compositor dies by mistake, it
// starts another one on the same socket: Qt and KDE apps, with
// QT_WAYLAND_RECONNECT=1, reconnect by themselves and show up again, as in
// Plasma when KWin restarts. The others (Chromium, Electron, GTK, X11) close.
//
// If the screen was locked, the new compositor starts locked (black, and
// vela-lock): a crash must never reveal the desktop. If the compositor crashes
// three times in a minute, the supervisor gives up and the session ends (back
// to the login screen).
//
// The supervisor adopts the apps started in the session (subreaper): when the
// session ends it closes those left, which would otherwise hang waiting for a
// compositor that never comes back.

#include "supervisor.h"
#include "error_screen.h"
#include "diagnostics.h"
#include "path_security.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CRASHES 3 // within CRASH_WINDOW seconds: then give up
#define CRASH_WINDOW 60.0

// The only two pieces of global state: the signal handlers touch them.
static volatile sig_atomic_t stop_requested;
static volatile pid_t child_pid;

static void on_terminate(int signal)
{
    stop_requested = 1;
    if (child_pid > 0) {
        kill(child_pid, signal);
    }
}

// The supervisor is not a compositor. In particular SIGUSR1 requests a
// renderer redraw, not a session shutdown. Use a handler (not SIG_IGN): the
// forked child must regain SIG_DFL at exec before Wayland registers signalfd.
static void on_compositor_redraw(int signal)
{
    (void)signal;
}

static void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "vela-supervise: ");
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

// The Wayland socket, like wl_display_add_socket_auto: the first free
// wayland-N, with its lock file. It belongs to the supervisor until the end.
struct wayland_socket {
    char name[32];
    char path[PATH_MAX];
    char lock_path[PATH_MAX];
    int fd;
    int lock_fd;
};

static bool create_socket(int runtime_fd, struct wayland_socket *s)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    for (int n = 0; n < 32; ++n) {
        snprintf(s->name, sizeof(s->name), "wayland-%d", n);
        int length = snprintf(s->lock_path, sizeof(s->lock_path), "%s.lock", s->name);
        if (length < 0 || (size_t)length >= sizeof(s->lock_path)) {
            return false;
        }
        snprintf(s->path, sizeof(s->path), "%s", s->name);
        s->lock_fd = openat(runtime_fd, s->lock_path,
            O_CREAT | O_CLOEXEC | O_RDWR | O_NOFOLLOW | O_NONBLOCK, S_IRUSR | S_IWUSR);
        if (s->lock_fd < 0) {
            continue;
        }
        // The lock must be an ordinary file owned by this session's UID;
        // O_NOFOLLOW also rejects pre-existing symlink lock files.
        struct stat lock_info;
        if (fstat(s->lock_fd, &lock_info) != 0 || !S_ISREG(lock_info.st_mode)
            || lock_info.st_uid != geteuid()) {
            close(s->lock_fd);
            continue;
        }
        if (flock(s->lock_fd, LOCK_EX | LOCK_NB) != 0) {
            close(s->lock_fd); // there is already a compositor on this number
            continue;
        }
        // Reclaim only a stale socket owned by us. Never unlink a regular
        // file, directory or symlink just because it has a Wayland name.
        struct stat previous;
        if (fstatat(runtime_fd, s->path, &previous, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISSOCK(previous.st_mode) || previous.st_uid != geteuid()
                || unlinkat(runtime_fd, s->path, 0) != 0) {
                close(s->lock_fd);
                continue;
            }
        } else if (errno != ENOENT) {
            close(s->lock_fd);
            continue;
        }
        s->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s->fd < 0) {
            close(s->lock_fd);
            return false;
        }
        // A bounded copy even if the runtime directory was supplied externally.
        int copied = snprintf(address.sun_path, sizeof(address.sun_path),
            "/proc/self/fd/%d/%s", runtime_fd, s->path);
        if (copied < 0 || (size_t)copied >= sizeof(address.sun_path)) {
            close(s->fd);
            close(s->lock_fd);
            return false;
        }
        if (bind(s->fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(s->fd, 128) != 0) {
            close(s->fd);
            close(s->lock_fd);
            continue;
        }
        return true;
    }
    return false;
}

// The adopted processes still alive (direct children, the compositor
// excluded): an allocated array, to free; *count how many.
static pid_t *children(int *count)
{
    *count = 0;
    DIR *proc = opendir("/proc");
    if (!proc) {
        return NULL;
    }
    pid_t *result = NULL;
    int capacity = 0;
    pid_t self = getpid();
    struct dirent *entry;
    while ((entry = readdir(proc))) {
        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0) {
            continue;
        }
        char stat_path[64];
        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", (int)pid);
        FILE *file = fopen(stat_path, "r");
        if (!file) {
            continue;
        }
        char line[1024] = { 0 };
        bool read = fgets(line, sizeof(line), file) != NULL;
        fclose(file);
        // "pid (name) state ppid ...": the name can contain spaces and
        // parentheses.
        const char *end = read ? strrchr(line, ')') : NULL;
        char state = 0;
        int parent = 0;
        if (!end || sscanf(end + 1, " %c %d", &state, &parent) != 2 || parent != self || state == 'Z') {
            continue;
        }
        if (*count == capacity) {
            capacity = capacity ? capacity * 2 : 16;
            result = realloc(result, (size_t)capacity * sizeof(*result));
        }
        result[(*count)++] = pid;
    }
    closedir(proc);
    return result;
}

static void reap_all(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

// End of the session: the adopted apps left are closed (SIGTERM, and after two
// seconds SIGKILL for those that didn't listen).
static void close_children(void)
{
    int count = 0;
    pid_t *left = children(&count);
    if (count == 0) {
        free(left);
        return;
    }
    say("closing the remaining apps (%d)", count);
    for (int i = 0; i < count; ++i) {
        kill(left[i], SIGTERM);
    }
    for (int round = 0; round < 20 && count > 0; ++round) {
        usleep(100000);
        reap_all();
        free(left);
        left = children(&count);
    }
    for (int i = 0; i < count; ++i) {
        kill(left[i], SIGKILL);
    }
    free(left);
    usleep(100000);
    reap_all();
}

static double now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

bool vela_session_hook_path(char *out, size_t size)
{
    char self[PATH_MAX] = { 0 };
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = '\0';
            int written = snprintf(out, size, "%s/vela-session-env", self);
            if (written > 0 && (size_t)written < size && access(out, X_OK) == 0) {
                return true;
            }
        }
    }
    int written = snprintf(out, size, "%s/vela-session-env", VELA_LIBEXECDIR);
    return written > 0 && (size_t)written < size && access(out, X_OK) == 0;
}

// Starts `program` and waits for it to end (without leaving a zombie).
static void run_and_wait(const char *program, const char *argument)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(program, program, argument, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

int vela_supervise(int argc, char **argv)
{
    // Both processes execute vela-compositor. Give the supervisor its own
    // /proc/<pid>/comm so `pkill -x vela-compositor` never logs out the user.
    // Keep a defensive SIGUSR1 handler for explicit PID/group signals.
    struct sigaction redraw = { .sa_handler = on_compositor_redraw };
    sigemptyset(&redraw.sa_mask);
    if (sigaction(SIGUSR1, &redraw, NULL) != 0) {
        say("can't install supervisor SIGUSR1 handler: %s", strerror(errno));
        return 1;
    }
    if (prctl(PR_SET_NAME, "vela-supervise", 0, 0, 0) != 0) {
        say("can't set supervisor process name: %s", strerror(errno));
        return 1;
    }

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    int runtime_fd = vela_runtime_dir_open(runtime_dir);
    if (runtime_fd < 0) {
        say("XDG_RUNTIME_DIR must be an absolute, private directory owned by this user");
        return 1;
    }
    struct wayland_socket socket = { 0 };
    if (!create_socket(runtime_fd, &socket)) {
        say("can't create the Wayland socket");
        close(runtime_fd);
        return 1;
    }
    say("socket %s; starting the compositor", socket.name);

    // The compositor: the same program, with the same arguments except
    // --supervise.
    char self[PATH_MAX] = { 0 };
    if (readlink("/proc/self/exe", self, sizeof(self) - 1) <= 0) {
        say("can't find my own executable");
        return 1;
    }
    char **arguments = calloc((size_t)argc + 1, sizeof(*arguments));
    int count = 0;
    arguments[count++] = self;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--supervise") != 0) {
            arguments[count++] = argv[i];
        }
    }
    arguments[count] = NULL;

    // Apps started in the session (by the shell, with a double fork) stay our
    // children: at the end of the session we know whom to close.
    prctl(PR_SET_CHILD_SUBREAPER, 1);

    struct sigaction action = { .sa_handler = on_terminate };
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);

    double crashes[MAX_CRASHES];
    int crash_count = 0;
    int exit_code = 0;
    for (bool restarted = false;; restarted = true) {
        pid_t pid = fork();
        if (pid < 0) {
            say("fork: %s", strerror(errno));
            exit_code = 1;
            break;
        }
        if (pid == 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            // The compositor inherits the Wayland socket and the exact
            // runtime-directory descriptor. It restores CLOEXEC on the latter
            // before launching any applications.
            if (fcntl(socket.fd, F_SETFD, 0) != 0
                || fcntl(runtime_fd, F_SETFD, 0) != 0) {
                _exit(127);
            }
            char fd[16];
            snprintf(fd, sizeof(fd), "%d", socket.fd);
            setenv("VELA_WAYLAND_SOCKET_FD", fd, 1);
            setenv("VELA_WAYLAND_DISPLAY", socket.name, 1);
            snprintf(fd, sizeof(fd), "%d", runtime_fd);
            setenv("VELA_RUNTIME_DIR_FD", fd, 1);
            if (restarted) {
                setenv("VELA_RESTARTED", "1", 1);
            }
            unsetenv("VELA_START_LOCKED");
            if (vela_lock_marker_present_at(runtime_fd, socket.name)) {
                setenv("VELA_START_LOCKED", "1", 1);
            }
            execv(self, arguments);
            _exit(127);
        }
        child_pid = pid;
        // Adopted apps that end are reaped too: only the compositor matters.
        int status = 0;
        double started = now_seconds(), next_sample = started;
        for (;;) {
            pid_t done = waitpid(-1, &status, WNOHANG);
            if (done == pid || (done < 0 && errno != EINTR)) {
                break;
            }
            double now = now_seconds();
            if (now >= next_sample) {
                struct vela_process_resources resources;
                if (vela_process_resources(pid, &resources)) {
                    // A compact record. No paths, window titles or app names.
                    fprintf(stderr, "vela-supervise: resources pid=%ld elapsed_s=%llu fds=%u "
                        "sync_file=%u dmabuf=%u eventfd=%u sockets=%u nofile=%llu rss_kib=%llu\n",
                        (long)pid, (unsigned long long)(now - started), resources.fds,
                        resources.sync_file, resources.dmabuf, resources.eventfd,
                        resources.sockets, resources.nofile, resources.rss_kib);
                }
                next_sample = now + 2.0;
            }
            if (done <= 0) {
                struct timespec pause = { .tv_nsec = 100000000 };
                nanosleep(&pause, NULL);
            }
        }
        child_pid = 0;

        if (stop_requested) {
            // An unsafe-lock exit may have signalled an older supervisor to
            // prevent restart; preserve the dedicated status on new versions.
            if (WIFEXITED(status) && WEXITSTATUS(status) == VELA_EXIT_LOCK_UNSAFE) {
                exit_code = VELA_EXIT_LOCK_UNSAFE;
            }
            break; // the session is closing (logout, shutdown)
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            break; // exited by itself: "Sign out" from the menu
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == VELA_EXIT_UNSUPPORTED_GPU) {
            say("Vulkan 1.4 or required dmabuf support is unavailable; returning to login");
            exit_code = VELA_EXIT_UNSUPPORTED_GPU;
            break; // restarting cannot make the GPU support Vulkan
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == VELA_EXIT_LOCK_UNSAFE) {
            say("secure lock state could not be persisted; ending session, never restarting unlocked");
            exit_code = VELA_EXIT_LOCK_UNSAFE;
            break;
        }
        if (WIFSIGNALED(status)) {
            say("the compositor crashed (%s)", strsignal(WTERMSIG(status)));
        } else {
            say("the compositor exited with an error (code %d)", WEXITSTATUS(status));
        }
        // Only the crashes of the last minute are kept.
        double t = now_seconds();
        int kept = 0;
        for (int i = 0; i < crash_count; ++i) {
            if (t - crashes[i] <= CRASH_WINDOW) {
                crashes[kept++] = crashes[i];
            }
        }
        crash_count = kept;
        crashes[crash_count++] = t;
        if (crash_count >= MAX_CRASHES) {
            say("the compositor keeps exiting: giving up, the session ends");
            exit_code = 1;
            break;
        }
        say("restarting it");
    }
    free(arguments);

    close_children();

    // If the compositor couldn't do it (crash), the session is detached from
    // systemd; if it already did, nothing happens.
    char hook[PATH_MAX];
    if (vela_session_hook_path(hook, sizeof(hook))) {
        setenv("WAYLAND_DISPLAY", socket.name, 1);
        run_and_wait(hook, "stop");
    }
    vela_lock_marker_remove_at(runtime_fd, socket.name);
    unlinkat(runtime_fd, socket.path, 0);
    close(socket.fd);
    unlinkat(runtime_fd, socket.lock_path, 0);
    close(socket.lock_fd);
    close(runtime_fd);
    return exit_code;
}
