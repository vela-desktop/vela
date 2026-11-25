// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "process.h"

#include "alloc/alloc.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/pidfd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>

// `path` in single quotes for sh, a ' in it as '\''.
static bool quote(char *command, size_t size, const char *path)
{
    if (size < 3) {
        return false;
    }
    size_t n = 0;
    command[n++] = '\'';
    for (const char *c = path; *c; ++c) {
        const char *piece = *c == '\'' ? "'\\''" : c;
        const size_t length = *c == '\'' ? 4 : 1;
        if (n + length + 2 > size) { // and the closing quote, the NUL
            return false;
        }
        memcpy(command + n, piece, length);
        n += length;
    }
    command[n++] = '\'';
    command[n] = '\0';
    return true;
}

bool vela_find_program(const char *name, const char *build_dir, char *command, size_t size)
{
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        return false;
    }
    self[n] = '\0';
    char *slash = strrchr(self, '/');
    if (slash) {
        *slash = '\0';
    }
    char path[PATH_MAX];
    if ((vela_format(path, sizeof(path), "%s/%s", self, name) && access(path, X_OK) == 0)
        || (vela_format(path, sizeof(path), "%s/../%s/%s", self, build_dir, name) && access(path, X_OK) == 0)) {
        return quote(command, size, path);
    }
    return vela_format(path, sizeof(path), "%s/%s", VELA_LIBEXECDIR, name) && access(path, X_OK) == 0
        && quote(command, size, path);
}

void vela_exec_shell(const char *command)
{
    setsid();
    // wlroots blocks some signals to handle them in its loop: the processes we
    // launch must start with a clean mask.
    sigset_t set;
    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, NULL);
    execl("/bin/sh", "/bin/sh", "-c", command, (char *)NULL);
    _exit(127);
}

void vela_spawn(const char *command)
{
    pid_t child = fork();
    if (child == 0) {
        if (fork() == 0) {
            vela_exec_shell(command);
        }
        _exit(0);
    }
    if (child > 0) {
        waitpid(child, NULL, 0);
    }
}

// ---------------------------------------------------------- supervision --

void vela_child_init(struct vela_child *child, const char *name, int death_signal, int final_exit_code)
{
    memset(child, 0, sizeof(*child));
    child->name = name;
    child->death_signal = death_signal;
    child->final_exit_code = final_exit_code;
    child->pid = -1;
    child->pidfd = -1;
    child->keep_fd = -1;
}

static void stop_watching(struct vela_child *child)
{
    if (child->source) {
        wl_event_source_remove(child->source);
        child->source = NULL;
    }
    if (child->pidfd >= 0) {
        close(child->pidfd);
    }
    child->pidfd = -1;
    child->pid = -1;
}

static int handle_exit(int fd, uint32_t mask, void *data)
{
    struct vela_child *child = data;
    int status = 0;
    waitpid(child->pid, &status, 0);
    double uptime = (vela_now_ns() - child->started_ns) / 1e9;
    stop_watching(child);
    char *command = child->command;
    child->command = NULL;
    if (child->exited) {
        free(command);
        child->exited(child, status);
        return 0;
    }

    // A clean exit (such as "already running") was intended.
    if (WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == child->final_exit_code)) {
        wlr_log(WLR_INFO, "%s \"%s\" exited (code %d)", child->name, command, WEXITSTATUS(status));
        free(command);
        return 0;
    }
    char reason[64];
    if (WIFSIGNALED(status)) {
        snprintf(reason, sizeof(reason), "signal %s", strsignal(WTERMSIG(status)));
    } else {
        snprintf(reason, sizeof(reason), "code %d", WEXITSTATUS(status));
    }
    // When it keeps exiting right after starting, restarting it is pointless.
    child->quick_crashes = uptime < 5.0 ? child->quick_crashes + 1 : 0;
    if (child->quick_crashes >= 3) {
        wlr_log(WLR_ERROR, "%s \"%s\" keeps exiting (%s): not restarting it", child->name, command, reason);
    } else {
        wlr_log(WLR_ERROR, "%s \"%s\" exited (%s): restarting it", child->name, command, reason);
        vela_child_start(child, child->loop, command);
    }
    free(command);
    return 0;
}

bool vela_child_start(struct vela_child *child, struct wl_event_loop *loop, const char *command)
{
    // A single fork, so the process stays our child and a pidfd in the Wayland
    // loop tells us when it ends.
    char *copy = vela_strdup(command);
    free(child->command);
    child->command = copy;
    child->loop = loop;
    const pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "Can't start \"%s\"", command);
        return false;
    }
    if (pid == 0) {
        // The child dies with the compositor: if the compositor crashes, the
        // supervisor starts another one with new children, and the old ones
        // must not reconnect (QT_WAYLAND_RECONNECT) and make duplicates.
        if (prctl(PR_SET_PDEATHSIG, child->death_signal) < 0 || getppid() != parent) {
            _exit(127);
        }
        if (child->keep_fd >= 0) {
            fcntl(child->keep_fd, F_SETFD, 0);
        }
        vela_exec_shell(command);
    }
    child->pid = pid;
    child->pidfd = pidfd_open(pid, 0);
    if (child->pidfd >= 0) {
        child->started_ns = vela_now_ns();
        child->source = wl_event_loop_add_fd(loop, child->pidfd, WL_EVENT_READABLE, handle_exit, child);
    }
    if (!child->source) {
        wlr_log_errno(WLR_ERROR, "Can't watch \"%s\": stopped", command);
        // The shell may already have started another process in its
        // session. The unreaped child still owns its PID during both sends.
        kill(pid, SIGKILL);
        kill(-pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) { }
        stop_watching(child);
        return false;
    }
    return true;
}

void vela_child_stop(struct vela_child *child)
{
    stop_watching(child);
    free(child->command);
    child->command = NULL;
}
