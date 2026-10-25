// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lock.h"
#include "lock_retry_policy.h"

#include "listen.h"
#include "a11y.h"
#include "switcher.h"
#include "snap.h"
#include "focus.h"
#include "config.h"
#include "error_screen.h"
#include "input.h"
#include "output.h"
#include "process.h"
#include "scene/scene.h"
#include "scene/surface.h"
#include "server.h"
#include "path_security.h"
#include "util.h"
#include "view.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>

#define SCREEN_OFF_AFTER_LOCK_MS 5000 // locked for inactivity: then it turns off

static int handle_respawn(void *data);

// A surface of the lock program, as large as its output.
struct lock_surface {
    struct vela_server *server;
    struct wlr_session_lock_surface_v1 *wlr;
    struct vela_tree *tree;
    struct vela_surface_node *surface_node;
    struct wl_listener map;
    struct wl_listener destroy;
};

// The layers the lock hides: all but its own.
static void set_desktop_enabled(struct vela_server *server, bool enabled)
{
    struct vela_layers *l = &server->layers;
    struct vela_tree *layers[] = {
        l->background, l->bottom, l->windows, l->windows_out, l->top, l->top_above_fullscreen,
        l->fullscreen, l->x11_popups, l->overlay, l->drag,
    };
    for (size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); ++i) {
        vela_node_set_enabled(&layers[i]->node, enabled);
    }
}

// An unsafe supervisor marker (or a broken retry timer) ends the session.
// First submit an opaque black frame for every active output. Committing a
 // frame isn't proof of physical scanout, so allow another few refresh cycles
 // before exit; a bounded fallback avoids hanging on a failed GPU/connector.
#define VELA_LOCK_FATAL_TIMEOUT_MS 2000
#define VELA_LOCK_FATAL_SCANOUT_GRACE_MS 200

static __attribute__((noreturn)) void lock_unsafe_exit(struct vela_lock *lock)
{
    // Older supervisors do not understand exit 77 and would restart unlocked.
    // Their existing SIGTERM handler ends the session instead. Only signal
    // the supervisor after blackout, immediately before our terminal exit.
    if (lock->server->runtime_dir_fd >= 0 && getenv("VELA_WAYLAND_SOCKET_FD") && getppid() > 1) {
        kill(getppid(), SIGTERM);
    }
    _exit(VELA_EXIT_LOCK_UNSAFE);
}

static int handle_lock_fatal_exit(void *data)
{
    lock_unsafe_exit(data);
}

static void lock_fatal_shutdown(struct vela_lock *lock)
{
    if (lock->fatal_exit_pending) {
        return;
    }
    lock->fatal_exit_pending = true;
    struct vela_server *server = lock->server;
    // Suppress any currently displayed lock-client surfaces as well: a
    // terminal failure must leave only the compositor's opaque backdrop.
    set_desktop_enabled(server, false);
    vela_node_set_enabled(&server->layers.lock->node, true);
    if (lock->lock) {
        struct wlr_session_lock_surface_v1 *surface;
        wl_list_for_each (surface, &lock->lock->surfaces, link) {
            struct lock_surface *mapped = surface->data;
            if (mapped) {
                vela_node_set_enabled(&mapped->tree->node, false);
            }
        }
    }
    wlr_seat_keyboard_clear_focus(server->seat);
    wlr_seat_pointer_clear_focus(server->seat);
    vela_lock_update_layout(lock);
    vela_scene_changed(server->scene);
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        if (!output->wlr->enabled || !output->powered) {
            continue;
        }
        lock->fatal_waiting = vela_grow(lock->fatal_waiting, &lock->fatal_waiting_capacity,
            lock->fatal_waiting_count + 1, sizeof(*lock->fatal_waiting));
        lock->fatal_waiting[lock->fatal_waiting_count++] = output;
        vela_output_schedule_frame(output);
    }
    if (lock->fatal_waiting_count == 0) {
        wlr_log(WLR_ERROR, "Lock: no active outputs to blank before terminating session");
        lock_unsafe_exit(lock);
    }
    lock->fatal_timer = wl_event_loop_add_timer(server->loop, handle_lock_fatal_exit, lock);
    if (!lock->fatal_timer ||
        wl_event_source_timer_update(lock->fatal_timer, VELA_LOCK_FATAL_TIMEOUT_MS) < 0) {
        wlr_log_errno(WLR_ERROR, "Lock: cannot schedule bounded session shutdown");
        lock_unsafe_exit(lock);
    }
    wlr_log(WLR_ERROR, "Lock: ending session after the black backdrop is committed");
}

// Own a pending locker launcher so a non-registering process cannot survive
// into the next attempt. The shell is launched in its own session.
static void lock_stop_launcher(struct vela_lock *lock)
{
    if (lock->launcher_pid <= 0) {
        return;
    }
    pid_t pid = lock->launcher_pid;
    lock->launcher_pid = 0;
    // Signal before reaping to prevent PID reuse across retries.
    if (kill(-pid, SIGKILL) < 0 && errno != ESRCH) {
        wlr_log_errno(WLR_ERROR, "Lock: failed to stop launcher process group");
    }
    if (kill(pid, SIGKILL) < 0 && errno != ESRCH) {
        wlr_log_errno(WLR_ERROR, "Lock: failed to stop launcher");
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

static void lock_start_launcher(struct vela_lock *lock, const char *command)
{
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "Lock: unable to launch locker");
        return; // retry already scheduled
    }
    if (pid == 0) {
        vela_exec_shell(command);
    }
    lock->launcher_pid = pid;
}

// All retry paths share this arm operation, including crashes after registration.
static bool lock_arm_retry(struct vela_lock *lock, int delay_ms)
{
    if (!lock->respawn) {
        lock->respawn = wl_event_loop_add_timer(lock->server->loop, handle_respawn, lock);
    }
    if (!lock->respawn || wl_event_source_timer_update(lock->respawn, delay_ms) < 0) {
        wlr_log_errno(WLR_ERROR, "Lock: retry timer unavailable; ending session safely");
        lock_fatal_shutdown(lock);
        return false;
    }
    lock->retry.scheduled = true;
    return true;
}

static void place_surface(struct lock_surface *surface)
{
    struct wlr_box box = { 0 };
    wlr_output_layout_get_box(surface->server->output_layout, surface->wlr->output, &box);
    vela_node_set_position(&surface->tree->node, box.x, box.y);
    if (box.width > 0 && box.height > 0) {
        wlr_session_lock_surface_v1_configure(surface->wlr, (uint32_t)box.width, (uint32_t)box.height);
    }
}

static void handle_surface_map(struct wl_listener *listener, void *data)
{
    struct lock_surface *surface = wl_container_of(listener, surface, map);
    struct vela_server *server = surface->server;
    // The keyboard goes to the output where the mouse is (or the first).
    struct vela_output *under = vela_output_under_cursor(server);
    if (!server->seat->keyboard_state.focused_surface || (under && under->wlr == surface->wlr->output)) {
        vela_input_keyboard_enter(server->input, surface->wlr->surface);
    }
    vela_scene_changed(server->scene);
}

static void handle_surface_destroy(struct wl_listener *listener, void *data)
{
    struct lock_surface *surface = wl_container_of(listener, surface, destroy);
    struct wlr_seat *seat = surface->server->seat;
    if (seat->keyboard_state.focused_surface == surface->wlr->surface) {
        wlr_seat_keyboard_clear_focus(seat);
    }
    wl_list_remove(&surface->map.link);
    wl_list_remove(&surface->destroy.link);
    vela_node_destroy(&surface->surface_node->node);
    vela_node_destroy(&surface->tree->node);
    free(surface);
}

static void handle_new_surface(struct wl_listener *listener, void *data)
{
    struct vela_lock *lock = wl_container_of(listener, lock, new_surface);
    struct wlr_session_lock_surface_v1 *wlr = data;
    struct lock_surface *surface = calloc(1, sizeof(*surface));
    surface->server = lock->server;
    surface->wlr = wlr;
    surface->tree = vela_tree_create(lock->server->layers.lock);
    surface->surface_node = vela_surface_node_create(surface->tree, wlr->surface);
    wlr->data = surface;
    place_surface(surface);
    vela_listen(&wlr->surface->events.map, &surface->map, handle_surface_map);
    vela_listen(&wlr->events.destroy, &surface->destroy, handle_surface_destroy);
}

static void clear_backdrop(struct vela_lock *lock)
{
    for (int i = 0; i < lock->backdrop_count; ++i) {
        vela_node_destroy(&lock->backdrop[i]->node);
    }
    lock->backdrop_count = 0;
}

static void handle_unlock(struct wl_listener *listener, void *data)
{
    struct vela_lock *lock = wl_container_of(listener, lock, unlock);
    struct vela_server *server = lock->server;
    if (lock->fatal_exit_pending) {
        return; // never show desktop after an unsafe lock transition
    }
    // Really unlocked: the desktop is back as it was.
    server->locked = false;
    if (server->runtime_dir_fd >= 0
        && !vela_lock_marker_remove_at(server->runtime_dir_fd, server->socket_name)) {
        wlr_log(WLR_ERROR, "Could not remove secure screen-lock marker");
    }
    set_desktop_enabled(server, true);
    vela_node_set_enabled(&server->layers.lock->node, false);
    clear_backdrop(lock);
    wlr_seat_keyboard_clear_focus(server->seat);
    vela_focus_refocus(server);
    vela_scene_changed(server->scene);
    wlr_log(WLR_INFO, "Screen unlocked");
}

static void handle_lock_destroy(struct wl_listener *listener, void *data)
{
    struct vela_lock *lock = wl_container_of(listener, lock, destroy);
    lock_stop_launcher(lock);
    // This wlroots lock is being torn down. A timer-arm failure may initiate
    // terminal blackout, which must never walk surfaces on the dying object.
    lock->lock = NULL;
    // If it didn't unlock (crash), everything stays locked and black: a new
    // vela-lock can take its place.
    if (lock->server->locked) {
        wlr_log(WLR_ERROR, "The locker went away without unlocking: staying locked");
        // It's relaunched in a second (at most 5 times a minute): otherwise
        // only the black screen would be left.
        int64_t now = vela_now_ns() / VELA_NS_PER_MS;
        int kept = 0;
        for (int i = 0; i < lock->respawn_count; ++i) {
            if (now - lock->respawns[i] <= 60000) {
                lock->respawns[kept++] = lock->respawns[i];
            }
        }
        lock->respawn_count = kept;
        if (lock->respawn_count < VELA_LOCK_RESPAWNS) {
            lock->respawns[lock->respawn_count++] = now;
            lock_arm_retry(lock, 1000);
        } else {
            // A repeatedly crashing locker remains black, but can recover
            // after a cooldown instead of requiring a new login.
            wlr_log(WLR_ERROR, "Lock: repeated crashes; retrying after cooldown");
            lock_arm_retry(lock, VELA_LOCK_RETRY_COOLDOWN_MS);
        }
    }
    lock->waiting_count = 0;
    wl_list_remove(&lock->new_surface.link);
    wl_list_remove(&lock->unlock.link);
    wl_list_remove(&lock->destroy.link);
    wl_list_init(&lock->new_surface.link);
    wl_list_init(&lock->unlock.link);
    wl_list_init(&lock->destroy.link);
}

static void handle_new_lock(struct wl_listener *listener, void *data)
{
    struct vela_lock *lock = wl_container_of(listener, lock, new_lock);
    struct vela_server *server = lock->server;
    struct wlr_session_lock_v1 *wlr = data;
    if (lock->fatal_exit_pending || lock->lock) {
        wlr_log(WLR_INFO, "Lock: refusing duplicate or unsafe locker registration");
        wlr_session_lock_v1_destroy(wlr);
        return;
    }
    lock->lock = wlr;
    // Establish the black, input-isolated state before any cancellation error
    // can enter the terminal failure path, including unsolicited clients.
    vela_lock_engage(lock);
    if (lock->fatal_exit_pending) {
        wlr_session_lock_v1_destroy(wlr);
        lock->lock = NULL;
        return;
    }
    vela_lock_retry_registered(&lock->retry);
    if (lock->respawn && wl_event_source_timer_update(lock->respawn, 0) < 0) {
        wlr_log_errno(WLR_ERROR, "Lock: could not cancel pending retry timer");
        lock_fatal_shutdown(lock);
        wlr_session_lock_v1_destroy(wlr);
        lock->lock = NULL;
        return;
    }
    // The app is told "locked" once every output has shown black.
    lock->waiting_count = 0;
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        if (output->wlr->enabled) {
            lock->waiting = vela_grow(lock->waiting, &lock->waiting_capacity, lock->waiting_count + 1,
                sizeof(*lock->waiting));
            lock->waiting[lock->waiting_count++] = output;
            vela_output_schedule_frame(output);
        }
    }
    if (lock->waiting_count == 0) {
        wlr_session_lock_v1_send_locked(wlr);
    }
    vela_scene_changed(server->scene);

    vela_listen(&wlr->events.new_surface, &lock->new_surface, handle_new_surface);
    vela_listen(&wlr->events.unlock, &lock->unlock, handle_unlock);
    vela_listen(&wlr->events.destroy, &lock->destroy, handle_lock_destroy);
}

static int handle_respawn(void *data)
{
    struct vela_lock *lock = data;
    lock_stop_launcher(lock);
    vela_lock_retry_timer_fired(&lock->retry);
    vela_lock_screen(lock);
    return 0;
}

// ------------------------------------------------------------- inactivity --

// An inactivity inhibitor (a video): when it ends the wait starts again.
struct inhibitor_watch {
    struct vela_lock *lock;
    struct wl_listener destroy;
};

static void handle_inhibitor_destroy(struct wl_listener *listener, void *data)
{
    struct inhibitor_watch *watch = wl_container_of(listener, watch, destroy);
    struct vela_lock *lock = watch->lock;
    wl_list_remove(&watch->destroy.link);
    free(watch);
    // When it ends, inactivity starts from zero.
    vela_lock_note_activity(lock);
}

static void handle_new_inhibitor(struct wl_listener *listener, void *data)
{
    struct vela_lock *lock = wl_container_of(listener, lock, new_inhibitor);
    struct wlr_idle_inhibitor_v1 *inhibitor = data;
    struct inhibitor_watch *watch = calloc(1, sizeof(*watch));
    watch->lock = lock;
    vela_listen(&inhibitor->events.destroy, &watch->destroy, handle_inhibitor_destroy);
    vela_lock_note_activity(lock);
}

static int handle_idle_timer(void *data)
{
    struct vela_lock *lock = data;
    struct vela_server *server = lock->server;
    // A timed caffeine that ran out while its timer was still queued.
    if (lock->caffeine && lock->caffeine_until_ns > 0 && vela_now_ns() >= lock->caffeine_until_ns) {
        vela_lock_set_caffeine(lock, false);
        vela_a11y_announce(server->a11y);
    }
    // An app is showing something (a video), or caffeine: try again later.
    bool inhibited = lock->caffeine;
    struct wlr_idle_inhibitor_v1 *inhibitor;
    wl_list_for_each (inhibitor, &lock->idle_inhibit->inhibitors, link) {
        const struct vela_surface_state *state = vela_surface_state_get(inhibitor->surface);
        inhibited = inhibited || (inhibitor->surface->mapped && state && state->pacing);
    }
    if (inhibited) {
        wl_event_source_timer_update(lock->idle_timer, lock->screen_off_ms);
        return 0;
    }
    if (lock->lock_on_idle && !server->locked && !lock->locking) {
        lock->locking = true;
        vela_lock_screen(lock);
        wl_event_source_timer_update(lock->idle_timer, SCREEN_OFF_AFTER_LOCK_MS);
        return 0;
    }
    lock->locking = false;
    lock->screens_off = true;
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        vela_output_set_powered(output, false);
    }
    return 0;
}

void vela_lock_load_settings(struct vela_lock *lock)
{
    if (!lock->idle_timer) {
        return; // nested or headless nothing turns off
    }
    struct vela_config settings;
    vela_config_read(&settings);
    const char *minutes = getenv("VELA_SCREEN_OFF");
    if (!minutes || !*minutes) {
        minutes = vela_config_get(&settings, "screen-off", "");
    }
    char *end = NULL;
    errno = 0;
    const double parsed = *minutes ? strtod(minutes, &end) : 10.0; // fractions for tests
    const bool valid = !*minutes || (end != minutes && *end == '\0' && errno != ERANGE
        && isfinite(parsed) && parsed >= 0 && parsed <= INT_MAX / 60000.0);
    if (!valid) {
        wlr_log(WLR_ERROR, "Invalid screen-off timeout: using 10 minutes");
    }
    const double wait = valid ? parsed : 10.0;
    lock->screen_off_ms = wait > 0 ? vela_max(1, (int)ceil(wait * 60000.0)) : 0;
    const char *on_idle = getenv("VELA_LOCK_ON_IDLE");
    if (!on_idle || !*on_idle) {
        on_idle = vela_config_get(&settings, "lock-on-idle", "");
    }
    lock->lock_on_idle = strcmp(on_idle, "0") != 0 && strcmp(on_idle, "no") != 0;
    vela_config_finish(&settings);
    wlr_log(WLR_INFO, "Idle: screen off after %d seconds%s", lock->screen_off_ms / 1000,
        lock->lock_on_idle ? ", with lock" : "");
    // The wait starts again from now, with the new minutes.
    lock->locking = false;
    wl_event_source_timer_update(lock->idle_timer, lock->screen_off_ms);
}

void vela_lock_note_activity(struct vela_lock *lock)
{
    struct vela_server *server = lock->server;
    if (lock->idle_notifier) {
        wlr_idle_notifier_v1_notify_activity(lock->idle_notifier, server->seat);
    }
    if (!lock->idle_timer) {
        return;
    }
    if (lock->screens_off) {
        lock->screens_off = false;
        struct vela_output *output;
        wl_list_for_each (output, &server->outputs, link) {
            vela_output_set_powered(output, true);
        }
    }
    lock->locking = false;
    if (lock->screen_off_ms > 0) {
        wl_event_source_timer_update(lock->idle_timer, lock->screen_off_ms);
    }
}

// Arm from the monotonic deadline, rounding up whole milliseconds.
static bool arm_caffeine_timer(struct vela_lock *lock, int64_t deadline_ns)
{
    if (!lock->caffeine_timer) {
        return false;
    }
    int64_t remaining = deadline_ns - vela_now_ns();
    if (remaining < 1) {
        remaining = 1;
    }
    const int ms = (int)((remaining + VELA_NS_PER_MS - 1) / VELA_NS_PER_MS);
    if (wl_event_source_timer_update(lock->caffeine_timer, ms) < 0) {
        wlr_log_errno(WLR_ERROR, "Could not arm caffeine expiry timer");
        return false;
    }
    return true;
}

void vela_lock_set_caffeine(struct vela_lock *lock, bool on)
{
    lock->caffeine = on;
    lock->caffeine_until_ns = 0;
    lock->caffeine_duration_s = 0;
    if (lock->caffeine_timer) {
        wl_event_source_timer_update(lock->caffeine_timer, 0);
    }
    if (lock->idle_notifier) {
        wlr_idle_notifier_v1_set_inhibited(lock->idle_notifier, on);
    }
    // Turning it on wakes the outputs; turning it off starts the wait from zero.
    vela_lock_note_activity(lock);
    wlr_log(WLR_INFO, "Caffeine %s", on ? "on" : "off");
}

bool vela_lock_set_caffeine_for(struct vela_lock *lock, double minutes)
{
    if (!isfinite(minutes) || minutes <= 0 || minutes > VELA_CAFFEINE_MAX_MINUTES) {
        wlr_log(WLR_ERROR, "Invalid caffeine duration");
        return false;
    }
    if (!lock->caffeine_timer) {
        wlr_log(WLR_ERROR, "Caffeine expiry timer unavailable; refusing timed inhibition");
        vela_lock_set_caffeine(lock, false); // A previous indefinite session must not survive.
        return false;
    }
    const int64_t duration_ns = (int64_t)ceil(minutes * 60.0 * VELA_NS_PER_SEC);
    const int64_t deadline_ns = vela_now_ns() + duration_ns;
    // A timed session must never become active without a working expiry
    // timer. If rearming fails, fail closed even when replacing old caffeine.
    if (!arm_caffeine_timer(lock, deadline_ns)) {
        vela_lock_set_caffeine(lock, false);
        return false;
    }
    lock->caffeine_until_ns = deadline_ns;
    lock->caffeine_duration_s = (int)ceil(minutes * 60.0);
    lock->caffeine = true;
    if (lock->idle_notifier) {
        wlr_idle_notifier_v1_set_inhibited(lock->idle_notifier, true);
    }
    vela_lock_note_activity(lock);
    wlr_log(WLR_INFO, "Caffeine on for %.2f minutes", minutes);
    return true;
}

int vela_lock_caffeine_remaining_s(struct vela_lock *lock)
{
    if (!lock->caffeine) {
        return 0;
    }
    if (lock->caffeine_until_ns <= 0) {
        return -1;
    }
    int64_t left = lock->caffeine_until_ns - vela_now_ns();
    if (left <= 0) {
        return 0;
    }
    return (int)((left + VELA_NS_PER_SEC - 1) / VELA_NS_PER_SEC);
}

static int handle_caffeine_timer(void *data)
{
    struct vela_lock *lock = data;
    if (!lock->caffeine || lock->caffeine_until_ns <= 0) {
        return 0;
    }
    if (vela_now_ns() < lock->caffeine_until_ns) {
        if (arm_caffeine_timer(lock, lock->caffeine_until_ns)) {
            return 0; // timer woke before the monotonic deadline
        }
        // Timer rearming failed: do not leave indefinite idle inhibition.
        vela_lock_set_caffeine(lock, false);
        vela_a11y_announce(lock->server->a11y);
        return 0;
    }
    vela_lock_set_caffeine(lock, false);
    vela_a11y_announce(lock->server->a11y);
    wlr_log(WLR_INFO, "Caffeine time is up");
    return 0;
}

// -------------------------------------------------------------- creation --

struct vela_lock *vela_lock_create(struct vela_server *server)
{
    struct vela_lock *lock = calloc(1, sizeof(*lock));
    lock->server = server;
    lock->lock_on_idle = true;
    vela_node_set_enabled(&server->layers.lock->node, false);
    // First the backdrop, then (above) the lock program's surfaces.
    lock->backdrop_tree = vela_tree_create(server->layers.lock);
    lock->respawn = wl_event_loop_add_timer(server->loop, handle_respawn, lock);
    if (!lock->respawn) {
        wlr_log_errno(WLR_ERROR, "Unable to create locker retry timer");
    }
    wl_list_init(&lock->new_surface.link);
    wl_list_init(&lock->unlock.link);
    wl_list_init(&lock->destroy.link);

    lock->manager = wlr_session_lock_manager_v1_create(server->display);
    vela_listen(&lock->manager->events.new_lock, &lock->new_lock, handle_new_lock);

    // Timed caffeine turns itself off (cheap in every session, tests too).
    lock->caffeine_timer = wl_event_loop_add_timer(server->loop, handle_caffeine_timer, lock);
    if (!lock->caffeine_timer) {
        wlr_log_errno(WLR_ERROR, "Unable to create caffeine expiry timer");
    }

    lock->idle_notifier = wlr_idle_notifier_v1_create(server->display);
    lock->idle_inhibit = wlr_idle_inhibit_v1_create(server->display);
    vela_listen(&lock->idle_inhibit->events.new_inhibitor, &lock->new_inhibitor, handle_new_inhibitor);

    // Only in the real session: nested or headless nothing turns off (tests
    // ask for it with VELA_SCREEN_OFF).
    const char *screen_off = getenv("VELA_SCREEN_OFF");
    if (server->session || (screen_off && *screen_off)) {
        lock->idle_timer = wl_event_loop_add_timer(server->loop, handle_idle_timer, lock);
        vela_lock_load_settings(lock);
    }
    return lock;
}

void vela_lock_destroy(struct vela_lock *lock)
{
    lock_stop_launcher(lock);
    if (!lock) {
        return;
    }
    if (lock->caffeine_timer) {
        wl_event_source_remove(lock->caffeine_timer);
    }
    if (lock->respawn) {
        wl_event_source_remove(lock->respawn);
    }
    if (lock->idle_timer) {
        wl_event_source_remove(lock->idle_timer);
    }
    if (lock->fatal_timer) {
        wl_event_source_remove(lock->fatal_timer);
    }
    free(lock->fatal_waiting);
    clear_backdrop(lock);
    vela_node_destroy(&lock->backdrop_tree->node);
    free(lock->backdrop);
    free(lock->waiting);
    wl_list_remove(&lock->new_lock.link);
    wl_list_remove(&lock->new_inhibitor.link);
    wl_list_remove(&lock->new_surface.link);
    wl_list_remove(&lock->unlock.link);
    wl_list_remove(&lock->destroy.link);
    free(lock);
}

// ------------------------------------------------------------------- lock --

/** Engage the screen lock and revoke any previously issued window-source captures. */
void vela_lock_engage(struct vela_lock *lock)
{
    struct vela_server *server = lock->server;
    if (server->locked) {
        return;
    }
    server->locked = true;
    // Cancel the active DnD grab before hiding the desktop. Ending the
    // matching wlroots grab invokes its cancellation path (not its drop path).
    // The drag may be destroyed by that callback: don't access it afterwards.
    if (server->seat->drag) {
        switch (server->seat->drag->grab_type) {
        case WLR_DRAG_GRAB_KEYBOARD_POINTER:
            wlr_seat_pointer_end_grab(server->seat);
            break;
        case WLR_DRAG_GRAB_KEYBOARD_TOUCH:
            wlr_seat_touch_end_grab(server->seat);
            break;
        case WLR_DRAG_GRAB_KEYBOARD:
            wlr_seat_keyboard_end_grab(server->seat);
            break;
        }
    }
    // Invalidate old isolated window sources; output capture still shows the
    // composed lock screen, but window capture could bypass occlusion.
    vela_views_revoke_capture_on_lock(server);
    // Before exposing the locked state to the supervisor, persist its marker.
    // If we cannot, a compositor restart might reveal the desktop: end the
    // entire session instead of ever allowing a restart without a marker.
    bool marker_failed = server->runtime_dir_fd >= 0
        && !vela_lock_marker_create_at(server->runtime_dir_fd, server->socket_name);
    set_desktop_enabled(server, false);
    vela_node_set_enabled(&server->layers.lock->node, true);
    server->cursor_mode = VELA_CURSOR_PASSTHROUGH;
    // No drags, Alt+Tab or panels with the keyboard.
    server->grabbed = NULL;
    vela_snap_end_zone(server, false);
    vela_switcher_finish(server, false);
    server->focused_layer = NULL;
    wlr_seat_keyboard_clear_focus(server->seat);
    wlr_seat_pointer_clear_focus(server->seat);
    wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
    vela_lock_update_layout(lock);
    vela_scene_changed(server->scene);
    wlr_log(WLR_INFO, "Screen locked");
    if (marker_failed) {
        wlr_log(WLR_ERROR, "Could not create secure screen-lock marker");
        lock_fatal_shutdown(lock);
    }
}

void vela_lock_screen(struct vela_lock *lock)
{
    if (lock->fatal_exit_pending || lock->lock) {
        return;
    }
    // Black backdrop and revoked focus/captures precede any locker launch.
    vela_lock_engage(lock);
    if (lock->fatal_exit_pending || !vela_lock_retry_begin(&lock->retry)) {
        return; // duplicate command while waiting for registration
    }
    // Never rely on fork() success: only ext-session-lock registration cancels
    // retries. After five failed launches, back off for 60 seconds and retry.
    if (!lock_arm_retry(lock, vela_lock_retry_delay_ms(&lock->retry))) {
        return;
    }
    // VELA_LOCK picks another program (such as swaylock). Otherwise vela-lock
    // next to the compositor (installed) or in the build directory.
    char command[PATH_MAX + 2] = "";
    const char *custom = getenv("VELA_LOCK");
    if (custom && *custom) {
        snprintf(command, sizeof(command), "%s", custom);
    } else {
        char self[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = '\0';
            char *slash = strrchr(self, '/');
            if (slash) {
                *slash = '\0';
            }
            const char *candidates[] = { "%s/vela-lock", "%s/../lock/vela-lock" };
            for (size_t i = 0; i < 2 && !command[0]; ++i) {
                char path[PATH_MAX];
                if (vela_format(path, sizeof(path), candidates[i], self) && access(path, X_OK) == 0) {
                    snprintf(command, sizeof(command), "'%s'", path);
                }
            }
        }
    }
    if (!command[0]) {
        wlr_log(WLR_ERROR, "Lock: vela-lock not found; keeping desktop hidden and retrying");
        return;
    }
    wlr_log(WLR_INFO, "Locking the screen: %s", command);
    lock_start_launcher(lock, command);
}

void vela_lock_output_rendered(struct vela_lock *lock, struct vela_output *output)
{
    if (lock->fatal_exit_pending && lock->fatal_waiting_count > 0) {
        for (int i = 0; i < lock->fatal_waiting_count; ++i) {
            if (lock->fatal_waiting[i] != output) {
                continue;
            }
            lock->fatal_waiting[i] = lock->fatal_waiting[--lock->fatal_waiting_count];
            if (lock->fatal_waiting_count == 0) {
                wlr_log(WLR_ERROR, "Lock: blackout committed on all active outputs; ending session");
                // Permit scanout to catch up with the committed black frames.
                if (!lock->fatal_timer ||
                    wl_event_source_timer_update(lock->fatal_timer, VELA_LOCK_FATAL_SCANOUT_GRACE_MS) < 0) {
                    lock_unsafe_exit(lock);
                }
            }
            break;
        }
    }
    if (!lock->lock || lock->waiting_count == 0) {
        return;
    }
    int kept = 0;
    for (int i = 0; i < lock->waiting_count; ++i) {
        if (lock->waiting[i] != output) {
            lock->waiting[kept++] = lock->waiting[i];
        }
    }
    lock->waiting_count = kept;
    if (lock->waiting_count == 0) {
        wlr_session_lock_v1_send_locked(lock->lock);
    }
}

void vela_lock_update_layout(struct vela_lock *lock)
{
    struct vela_server *server = lock->server;
    if (!server->locked) {
        return;
    }
    // A black backdrop per output, below the lock program's surfaces: even
    // before it draws, or when it's gone, nothing else shows.
    clear_backdrop(lock);
    const struct wlr_render_color black = { 0.0f, 0.0f, 0.0f, 1.0f };
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        struct wlr_box box = vela_output_box(output);
        if (wlr_box_empty(&box)) {
            continue;
        }
        struct vela_rect_node *rect = vela_rect_node_create(lock->backdrop_tree, box.width, box.height, &black);
        vela_node_set_position(&rect->node, box.x, box.y);
        lock->backdrop = vela_grow(lock->backdrop, &lock->backdrop_capacity, lock->backdrop_count + 1,
            sizeof(*lock->backdrop));
        lock->backdrop[lock->backdrop_count++] = rect;
    }
    if (lock->lock) {
        struct wlr_session_lock_surface_v1 *surface;
        wl_list_for_each (surface, &lock->lock->surfaces, link) {
            if (surface->data) {
                place_surface(surface->data);
            }
        }
    }
}
