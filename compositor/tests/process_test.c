// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Children of the compositor (process.c), on a real event loop, no GPU: a
// child's exit reaches its callback, including through a reserved fallback
// under descriptor pressure; automatic restarts survive a failed watch/fork.

#undef NDEBUG // CI uses RelWithDebInfo: assertions must remain active.
#include "process.h"

// Both supervision and output latching must survive event-source failures.
// Function sections discard unrelated output/rendering code: no GPU is used.
#include "../src/output.c"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

static int fail_watches;
static int fail_forks;
static int fail_timers;

struct wl_event_source *__real_wl_event_loop_add_fd(struct wl_event_loop *, int, uint32_t,
    wl_event_loop_fd_func_t, void *);
struct wl_event_source *__wrap_wl_event_loop_add_fd(struct wl_event_loop *loop, int fd, uint32_t mask,
    wl_event_loop_fd_func_t callback, void *data)
{
    if (fail_watches) {
        --fail_watches;
        errno = ENOMEM;
        return NULL;
    }
    return __real_wl_event_loop_add_fd(loop, fd, mask, callback, data);
}

struct wl_event_source *__real_wl_event_loop_add_timer(struct wl_event_loop *,
    wl_event_loop_timer_func_t, void *);
struct wl_event_source *__wrap_wl_event_loop_add_timer(struct wl_event_loop *loop,
    wl_event_loop_timer_func_t callback, void *data)
{
    if (fail_timers) {
        --fail_timers;
        errno = ENOMEM;
        return NULL;
    }
    return __real_wl_event_loop_add_timer(loop, callback, data);
}

static void initial_watch_failure_is_reported(struct wl_event_loop *loop)
{
    struct vela_child child;
    vela_child_init(&child, "test-reservation", SIGTERM, -1);
    fail_timers = 1;
    assert(!vela_child_start(&child, loop, "exit 0"));
    assert(!fail_timers && !child.fallback && child.pid == -1 && !child.command);
    // The caller can reject incomplete startup; no untracked process exists.
    vela_child_stop(&child);
}

pid_t __real_fork(void);
pid_t __wrap_fork(void)
{
    if (fail_forks) {
        --fail_forks;
        errno = EAGAIN;
        return -1;
    }
    return __real_fork();
}

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

static void fallback_reports_a_child_under_descriptor_pressure(struct wl_event_loop *loop)
{
    struct rlimit saved;
    assert(getrlimit(RLIMIT_NOFILE, &saved) == 0);
    struct rlimit low = { (rlim_t)lowest_free_descriptor() + 1, saved.rlim_max };
    assert(setrlimit(RLIMIT_NOFILE, &low) == 0);
    struct vela_child child;
    vela_child_init(&child, "test", SIGTERM, -1);
    child.exited = exited;
    bool started = vela_child_start(&child, loop, "exit 7");
    assert(setrlimit(RLIMIT_NOFILE, &saved) == 0);
    assert(started && child.pid > 0 && !child.source && child.fallback);
    for (int i = 0; i < 100 && exits < 2; ++i) {
        wl_event_loop_dispatch(loop, 50);
    }
    assert(exits == 2 && WIFEXITED(last_status) && WEXITSTATUS(last_status) == 7);
    assert(child.pid == -1 && child.pidfd == -1 && !child.source);
    vela_child_stop(&child);
    assert(!child.fallback);
    errno = 0;
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
}

static void automatic_relaunch_survives_setup_failure(struct wl_event_loop *loop, bool fork_fails)
{
    struct vela_child child;
    vela_child_init(&child, "test-auto", SIGTERM, -1);
    assert(vela_child_start(&child, loop, "exit 3"));
    if (fork_fails) {
        fail_forks = 1;
    } else {
        fail_watches = 1;
    }
    for (int i = 0; i < 100 && child.command; ++i) {
        wl_event_loop_dispatch(loop, 50);
    }
    assert(!fail_forks && !fail_watches);
    // The original three-quick-crash limit still applies after a recovered
    // setup failure. A lost restart would leave command non-NULL and idle.
    assert(child.quick_crashes == 3 && !child.command && child.pid == -1);
    wl_event_loop_dispatch(loop, 0);
    struct pollfd idle = { .fd = wl_event_loop_get_fd(loop), .events = POLLIN };
    assert(poll(&idle, 1, 150) == 0); // no fallback wakeups after supervision ends
    vela_child_stop(&child);
}

static int latch_ready(int fd, uint32_t mask, void *data)
{
    (void)fd; (void)mask; (void)data;
    return 0;
}

static void latch_failures_leave_frames_unarmed(struct wl_event_loop *loop)
{
    struct vela_output output = { .latch_fd = -1 };
    assert(!arm_latch(&output, vela_now_ns() + 1000000000));
    assert(!output.latch_armed);
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    assert(fd >= 0);
    output.latching = true;
    output.latch_fd = fd;
    output.latch_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, latch_ready, NULL);
    assert(output.latch_source);
    assert(arm_latch(&output, vela_now_ns() + 1000000000));
    assert(output.latch_armed);
    output.latch_armed = false;
    struct wl_event_source *source = output.latch_source;
    output.latch_source = NULL;
    assert(!arm_latch(&output, vela_now_ns() + 1000000000));
    assert(!output.latch_armed);
    output.latch_source = source;
    int wrong_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(wrong_fd >= 0);
    output.latch_fd = wrong_fd;
    assert(!arm_latch(&output, vela_now_ns() + 1000000000));
    assert(!output.latch_armed && !output.latching);
    close(wrong_fd);
    wl_event_source_remove(source);
    close(fd);
}

static void empty_animation_retries_wait_for_the_output_beat(struct wl_event_loop *loop)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    assert(fd >= 0);
    struct wlr_output wlr = { 0 };
    struct vela_output output = { .wlr = &wlr, .vblank_fd = fd, .latch_fd = -1 };
    vela_frame_clock_init(&output.clock);
    vela_frame_clock_set_mode_refresh(&output.clock, 180000);
    int64_t now = vela_now_ns();
    output.last_vblank = now;
    output.empty_until = now + vela_frame_clock_period(&output.clock);
    // A callback already queued before the empty frame must defer too. Repeated
    // requests share one timer, without an immediate idle or latch callback.
    for (int i = 0; i < 100; ++i) {
        assert(defer_animation_frame(&output, now + i * 1000));
        assert(output.vblank_armed && !output.idle_frame && !output.latch_armed);
    }
    struct itimerspec armed;
    assert(timerfd_gettime(fd, &armed) == 0);
    assert(armed.it_value.tv_sec || armed.it_value.tv_nsec);
    // Real damage, a backend commit, or a working latch may run immediately.
    output.frame_requested = true;
    assert(!defer_animation_frame(&output, now));
    output.frame_requested = false;
    wlr.needs_frame = true;
    assert(!defer_animation_frame(&output, now));
    wlr.needs_frame = false;
    output.latching = true;
    output.latch_fd = fd;
    output.latch_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, latch_ready, NULL);
    assert(output.latch_source);
    assert(!defer_animation_frame(&output, now));
    wl_event_source_remove(output.latch_source);
    output.latch_source = NULL;
    // Latching configured but unavailable still follows the output beat.
    assert(defer_animation_frame(&output, now));
    assert(!defer_animation_frame(&output, output.empty_until));
    assert(!defer_animation_frame(&output, output.empty_until + 1));
    close(fd);
}

int main(void)
{
    struct wl_event_loop *loop = wl_event_loop_create();
    assert(loop);
    initial_watch_failure_is_reported(loop);
    exit_reaches_the_callback(loop);
    fallback_reports_a_child_under_descriptor_pressure(loop);
    automatic_relaunch_survives_setup_failure(loop, false);
    automatic_relaunch_survives_setup_failure(loop, true);
    latch_failures_leave_frames_unarmed(loop);
    empty_animation_retries_wait_for_the_output_beat(loop);
    wl_event_loop_destroy(loop);
    puts("process: ok");
    return 0;
}
