// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Children of the compositor (process.c), on a real event loop, no GPU: a
// child's exit reaches its callback, and a child whose pidfd can't be
// watched (descriptors exhausted) is stopped and reaped instead of staying
// unwatched (docs/memory-allocation-policy.md, point 2b).

#undef NDEBUG // CI uses RelWithDebInfo: assertions must remain active.
#include "process.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

static int exits;
static int last_status;

static void exited(struct vela_child *child, int status)
{
    (void)child;
    ++exits;
    last_status = status;
}

static int lowest_free_descriptor(void)
{
    int fd = 0;
    while (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
        ++fd;
    }
    return fd;
}

static void exit_reaches_the_callback(struct wl_event_loop *loop)
{
    struct vela_child child;
    vela_child_init(&child, "test", SIGTERM, -1);
    child.exited = exited;
    assert(vela_child_start(&child, loop, "exit 3"));
    assert(child.pid > 0 && child.source);
    for (int i = 0; i < 100 && !exits; ++i) {
        wl_event_loop_dispatch(loop, 50);
    }
    assert(exits == 1);
    assert(WIFEXITED(last_status) && WEXITSTATUS(last_status) == 3);
    assert(child.pid == -1 && child.pidfd == -1 && !child.source);
    vela_child_stop(&child);
}

static void an_unwatched_child_is_reaped(struct wl_event_loop *loop)
{
    struct rlimit saved;
    assert(getrlimit(RLIMIT_NOFILE, &saved) == 0);
    // Room for the pidfd alone: its watch needs a duplicate, which fails.
    struct rlimit low = { (rlim_t)lowest_free_descriptor() + 1, saved.rlim_max };
    assert(setrlimit(RLIMIT_NOFILE, &low) == 0);
    struct vela_child child;
    vela_child_init(&child, "test", SIGTERM, -1);
    child.exited = exited;
    bool started = vela_child_start(&child, loop, "exec sleep 30");
    assert(setrlimit(RLIMIT_NOFILE, &saved) == 0);
    assert(!started);
    assert(child.pid == -1 && child.pidfd == -1 && !child.source);
    // Reaped already: no child is left, not even a zombie.
    errno = 0;
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    assert(exits == 1); // its exit was not reported as one of the child's
    vela_child_stop(&child);
}

int main(void)
{
    struct wl_event_loop *loop = wl_event_loop_create();
    assert(loop);
    exit_reaches_the_callback(loop);
    an_unwatched_child_is_reaped(loop);
    wl_event_loop_destroy(loop);
    puts("process: ok");
    return 0;
}
