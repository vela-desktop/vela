// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lock.h"

#include "listen.h"
#include "switcher.h"
#include "snap.h"
#include "focus.h"
#include "config.h"
#include "input.h"
#include "output.h"
#include "process.h"
#include "scene/scene.h"
#include "scene/surface.h"
#include "server.h"
#include "supervisor.h"
#include "util.h"
#include "view.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>

#define SCREEN_OFF_AFTER_LOCK_MS 5000 // locked for inactivity: then it turns off

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
        l->background, l->bottom, l->windows, l->windows_out, l->top, l->fullscreen, l->x11_popups, l->overlay,
    };
    for (size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); ++i) {
        vela_node_set_enabled(&layers[i]->node, enabled);
    }
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
    // Really unlocked: the desktop is back as it was.
    server->locked = false;
    char flag[PATH_MAX];
    if (vela_lock_flag_path(getenv("WAYLAND_DISPLAY"), flag, sizeof(flag))) {
        unlink(flag);
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
            wl_event_source_timer_update(lock->respawn, 1000);
        }
    }
    lock->lock = NULL;
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
    if (lock->lock) {
        wlr_log(WLR_INFO, "Lock: a locker is already running, rejecting the second one");
        wlr_session_lock_v1_destroy(wlr);
        return;
    }
    lock->lock = wlr;
    vela_lock_engage(lock);
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
    vela_lock_screen(data);
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
    // An app is showing something (a video): try again later.
    bool inhibited = false;
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
    lock->screen_off_ms = vela_max(0, *minutes ? atoi(minutes) : 10) * 60 * 1000;
    const char *on_idle = getenv("VELA_LOCK_ON_IDLE");
    if (!on_idle || !*on_idle) {
        on_idle = vela_config_get(&settings, "lock-on-idle", "");
    }
    lock->lock_on_idle = strcmp(on_idle, "0") != 0 && strcmp(on_idle, "no") != 0;
    vela_config_finish(&settings);
    wlr_log(WLR_INFO, "Idle: screen off after %d minutes%s", lock->screen_off_ms / 60000,
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
    wl_list_init(&lock->new_surface.link);
    wl_list_init(&lock->unlock.link);
    wl_list_init(&lock->destroy.link);

    lock->manager = wlr_session_lock_manager_v1_create(server->display);
    vela_listen(&lock->manager->events.new_lock, &lock->new_lock, handle_new_lock);

    lock->idle_notifier = wlr_idle_notifier_v1_create(server->display);
    lock->idle_inhibit = wlr_idle_inhibit_v1_create(server->display);
    vela_listen(&lock->idle_inhibit->events.new_inhibitor, &lock->new_inhibitor, handle_new_inhibitor);

    // Only in the real session: nested or headless nothing turns off.
    if (server->session) {
        lock->idle_timer = wl_event_loop_add_timer(server->loop, handle_idle_timer, lock);
        vela_lock_load_settings(lock);
    }
    return lock;
}

void vela_lock_destroy(struct vela_lock *lock)
{
    if (!lock) {
        return;
    }
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
    // Invalidate old isolated window sources; output capture still shows the
    // composed lock screen, but window capture could bypass occlusion.
    vela_views_revoke_capture_on_lock(server);
    // For the supervisor: if the compositor crashes now, the next one starts
    // locked (supervisor.c).
    char flag[PATH_MAX];
    if (vela_lock_flag_path(getenv("WAYLAND_DISPLAY"), flag, sizeof(flag))) {
        int fd = open(flag, O_CREAT | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (fd >= 0) {
            close(fd);
        }
    }
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
}

void vela_lock_screen(struct vela_lock *lock)
{
    if (lock->server->locked && lock->lock) {
        return; // already locked, and the lock program is there
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
        wlr_log(WLR_ERROR, "Lock: vela-lock not found (VELA_LOCK picks another locker)");
        return;
    }
    wlr_log(WLR_INFO, "Locking the screen: %s", command);
    vela_spawn(command);
}

void vela_lock_output_rendered(struct vela_lock *lock, struct vela_output *output)
{
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
