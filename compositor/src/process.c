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
        return vela_format(command, size, "'%s'", path);
    }
    return vela_format(path, sizeof(path), "%s/%s", VELA_LIBEXECDIR, name) && access(path, X_OK) == 0
        && vela_format(command, size, "'%s'", path);
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

static int handle_fallback(void *data);

static void schedule_fallback(struct vela_child *child, int delay_ms)
{
    // Its heap entry was reserved before fork. Losing this timer means the
    // event loop can no longer supervise the process safely.
    if (wl_event_source_timer_update(child->fallback, delay_ms) < 0) {
        wlr_log_errno(WLR_ERROR, "Can't arm %s's reserved child watch", child->name);
        abort();
    }
}

static int handle_exit(int fd, uint32_t mask, void *data)
{
    struct vela_child *child = data;
    if (child->pid <= 0) {
        return 0;
    }
    int status = 0;
    pid_t reaped;
    do {
        reaped = waitpid(child->pid, &status, WNOHANG);
    } while (reaped < 0 && errno == EINTR);
    if (reaped == 0) {
        return 0;
    }
    if (reaped < 0) {
        wlr_log_errno(WLR_ERROR, "Can't collect %s's exit status", child->name);
        status = EXIT_FAILURE << 8;
    }
    double uptime = (vela_now_ns() - child->started_ns) / 1e9;
    stop_watching(child);
    schedule_fallback(child, INT_MAX);
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
        if (!vela_child_start(child, child->loop, command)) {
            wlr_log(WLR_ERROR, "%s: relaunch pending on its reserved timer", child->name);
        }
    }
    free(command);
    return 0;
}

static int handle_fallback(void *data)
{
    struct vela_child *child = data;
    // Re-arm before an exit callback: its owner may free the child there.
    int delay = child->source || (child->pid <= 0 && !child->command)
        ? INT_MAX : (child->pid > 0 ? 100 : 1000);
    schedule_fallback(child, delay);
    if (child->pid > 0) {
        return handle_exit(-1, 0, child);
    }
    if (child->command) {
        char *command = child->command;
        child->command = NULL;
        vela_child_start(child, child->loop, command);
        free(command);
    }
    return 0;
}

bool vela_child_start(struct vela_child *child, struct wl_event_loop *loop, const char *command)
{
    // Reserve the fallback while no new child exists. Keep a heap slot armed
    // far in the future when pidfds work; fallback does not wake a healthy
    // session periodically, and needs no allocation under descriptor pressure.
    child->loop = loop;
    if (!child->fallback) {
        child->fallback = wl_event_loop_add_timer(loop, handle_fallback, child);
        if (!child->fallback || wl_event_source_timer_update(child->fallback, INT_MAX) < 0) {
            wlr_log_errno(WLR_ERROR, "Can't reserve %s's child watch", child->name);
            if (child->fallback) {
                wl_event_source_remove(child->fallback);
                child->fallback = NULL;
            }
            return false;
        }
    } else {
        schedule_fallback(child, INT_MAX);
    }
    char *copy = vela_strdup(command);
    free(child->command);
    child->command = copy;
    child->loop = loop;
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "Can't start \"%s\"", copy);
        schedule_fallback(child, 1000);
        return false;
    }
    if (pid == 0) {
        // The child dies with the compositor: if the compositor crashes, the
        // supervisor starts another one with new children, and the old ones
        // must not reconnect (QT_WAYLAND_RECONNECT) and make duplicates.
        prctl(PR_SET_PDEATHSIG, child->death_signal);
        if (child->keep_fd >= 0) {
            fcntl(child->keep_fd, F_SETFD, 0);
        }
        vela_exec_shell(copy);
    }
    child->pid = pid;
    child->started_ns = vela_now_ns();
    child->pidfd = pidfd_open(pid, 0);
    child->source = child->pidfd >= 0
        ? wl_event_loop_add_fd(loop, child->pidfd, WL_EVENT_READABLE, handle_exit, child) : NULL;
    if (!child->source) {
        wlr_log_errno(WLR_ERROR, "Can't install %s's pidfd watch; using its reserved timer", child->name);
        if (child->pidfd >= 0) {
            close(child->pidfd);
            child->pidfd = -1;
        }
        schedule_fallback(child, 100);
    }
    return true;
}

void vela_child_stop(struct vela_child *child)
{
    stop_watching(child);
    if (child->fallback) {
        wl_event_source_remove(child->fallback);
        child->fallback = NULL;
    }
    free(child->command);
    child->command = NULL;
}
