// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "autoscroll.h"

#include "config.h"
#include "focus.h"
#include "input.h"
#include "interact.h"
#include "listen.h"
#include "output.h"
#include "scene/scene.h"
#include "server.h"
#include "switcher.h"
#include "util.h"
#include "view.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

// Released within this distance (px) and time of the press: a click, the mode
// stays on; otherwise it was a drag and ends with the release.
#define CLICK_DISTANCE 4.0
#define CLICK_MS 500
// autoscroll-click=no: the press waits this long (or CLICK_DISTANCE of
// motion) before it becomes a held autoscroll; released earlier, it reaches
// the app after all (paste, new tab).
#define HOLD_MS 200
// v = K * s * (d - dead)^EXPONENT px/s, s = 1.25^(speed - 5), at most MAX.
#define SPEED_K 1.5
#define SPEED_EXPONENT 1.3
#define SPEED_MAX 12000.0
// A frame that came very late doesn't turn into a jump.
#define MAX_STEP_NS (50 * VELA_NS_PER_MS)

struct vela_autoscroll {
    struct vela_server *server;
    bool enabled; // middle-click-autoscroll
    int speed; // autoscroll-speed, 1-10
    bool click; // autoscroll-click: a middle click turns it on until the next click
    bool hold; // autoscroll-hold: held and dragged, it scrolls until the release

    // autoscroll-click=no: the middle press is held back until it is clearly
    // a drag (it starts) or a click (press and release go to the app).
    bool pending;
    uint32_t pending_time; // the press's

    bool active;
    bool held; // the middle button that started it is still down
    bool moved; // ...and went farther than a click
    int64_t pressed_at;
    double anchor_x, anchor_y;
    // The window it scrolls: compared only, never dereferenced (the surface
    // is watched and the view is the one with the keyboard).
    struct wlr_surface *surface;
    struct vela_view *view;
    struct vela_output *output; // whose vblank grid it follows
    struct wl_listener surface_destroy;
    double carry[2]; // what wl_fixed (1/256 px) couldn't carry yet, per axis
    bool scrolled[2]; // events sent since the start: axis_stop at the end
    int64_t last_tick; // on the vblank grid
    int64_t next_tick;
    // Presses it took: their releases are taken too (bit: button - BTN_MOUSE).
    // Track every Linux button code, including BTN_TRIGGER_HAPPY* and stylus
    // buttons: any press we consume must have its release consumed as well.
    uint32_t swallowed[(KEY_MAX / 32) + 1];
    bool escape_swallowed;

    int timer_fd;
    struct wl_event_source *timer;
};

static int64_t max64(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

static bool valid_button(uint32_t button)
{
    return button >= BTN_MOUSE && button <= KEY_MAX;
}

static void swallowed_add(struct vela_autoscroll *autoscroll, uint32_t button)
{
    if (valid_button(button)) {
        autoscroll->swallowed[button / 32] |= UINT32_C(1) << (button % 32);
    }
}

static bool swallowed_take(struct vela_autoscroll *autoscroll, uint32_t button)
{
    if (!valid_button(button)) {
        return false;
    }
    uint32_t mask = UINT32_C(1) << (button % 32);
    uint32_t *word = &autoscroll->swallowed[button / 32];
    bool consumed = (*word & mask) != 0;
    *word &= ~mask;
    return consumed;
}

static void swallowed_remove(struct vela_autoscroll *autoscroll, uint32_t button)
{
    (void)swallowed_take(autoscroll, button);
}

// ------------------------------------------------------------- the curve --

double vela_autoscroll_speed(double offset, int speed_setting)
{
    double distance = fabs(offset) - VELA_AUTOSCROLL_DEAD_ZONE;
    if (distance <= 0.0) {
        return 0.0;
    }
    double factor = pow(1.25, vela_clamp(speed_setting, 1, 10) - 5);
    return copysign(fmin(SPEED_K * factor * pow(distance, SPEED_EXPONENT), SPEED_MAX), offset);
}

// -------------------------------------------------------------- settings --

void vela_autoscroll_load_settings(struct vela_autoscroll *autoscroll)
{
    struct vela_config settings;
    vela_config_read(&settings);
    autoscroll->enabled = vela_config_flag(&settings, "middle-click-autoscroll", true);
    autoscroll->speed = vela_clamp(atoi(vela_config_get(&settings, "autoscroll-speed", "5")), 1, 10);
    autoscroll->click = vela_config_flag(&settings, "autoscroll-click", true);
    autoscroll->hold = vela_config_flag(&settings, "autoscroll-hold", true);
    vela_config_finish(&settings);
}

// Terminals paste with the middle button: left out.
static bool is_terminal(const char *app)
{
    const char *names[] = { "konsole", "org.kde.konsole", "foot", "footclient", "kitty", "alacritty",
        "vela-terminal", "xterm" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcasecmp(app, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool allowed_for(struct vela_view *view)
{
    const char *app = vela_view_app_id(view);
    return !is_terminal(app);
}

// ----------------------------------------------------------------- timer --

static void arm(struct vela_autoscroll *autoscroll, int64_t when)
{
    const struct itimerspec spec = { .it_value = vela_ns_timespec(when) };
    timerfd_settime(autoscroll->timer_fd, TFD_TIMER_ABSTIME, &spec, NULL);
}

static void disarm(struct vela_autoscroll *autoscroll)
{
    const struct itimerspec spec = { 0 };
    timerfd_settime(autoscroll->timer_fd, 0, &spec, NULL);
}

// The next vblank of the output after `after`, on the frame clock's grid
// (the real presentations); without one yet, a period after the last tick.
static int64_t next_vblank(struct vela_autoscroll *autoscroll, int64_t after)
{
    const struct vela_frame_clock *clock = &autoscroll->output->clock;
    int64_t period = vela_frame_clock_period(clock);
    if (clock->last_present <= 0) {
        return max64(autoscroll->last_tick + period, after);
    }
    // Half a period of slack: a tick that woke a little late still lands on
    // the next vblank, not the one after.
    return vela_frame_clock_vblank_after(clock, max64(after, autoscroll->last_tick + period / 2));
}

// ------------------------------------------------------------ start, stop --

static void activate(struct vela_autoscroll *autoscroll);
static void cancel_pending(struct vela_autoscroll *autoscroll, bool click);

static void stop(struct vela_autoscroll *autoscroll)
{
    if (!autoscroll->active) {
        return;
    }
    struct vela_server *server = autoscroll->server;
    struct wlr_seat *seat = server->seat;
    autoscroll->active = false;
    autoscroll->held = false;
    disarm(autoscroll);
    vela_unlisten(&autoscroll->surface_destroy);
    uint32_t ms = (uint32_t)(vela_now_ns() / VELA_NS_PER_MS);
    if (autoscroll->surface && seat->pointer_state.focused_surface == autoscroll->surface) {
        // axis_stop (value 0) for the axes that moved.
        bool any = false;
        for (int axis = 0; axis < 2; ++axis) {
            if (autoscroll->scrolled[axis]) {
                wlr_seat_pointer_notify_axis(seat, ms,
                    axis ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL, 0.0, 0,
                    WL_POINTER_AXIS_SOURCE_CONTINUOUS, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
                any = true;
            }
        }
        if (any) {
            wlr_seat_pointer_notify_frame(seat);
        }
        // Leave and enter again: the app sets its cursor back and learns
        // where the pointer is now.
        wlr_seat_pointer_clear_focus(seat);
    }
    autoscroll->surface = NULL;
    autoscroll->view = NULL;
    autoscroll->output = NULL;
    wlr_log(WLR_DEBUG, "Autoscroll: off");
    // Until the app (entered again below) sets its own.
    wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
    vela_interact_motion(server, ms);
}

static bool output_alive(struct vela_server *server, struct vela_output *output)
{
    struct vela_output *each;
    wl_list_for_each (each, &server->outputs, link) {
        if (each == output) {
            return true;
        }
    }
    return false;
}

// Whatever must end it, checked every frame (so nothing else has to call in).
static bool still_valid(struct vela_autoscroll *autoscroll)
{
    struct vela_server *server = autoscroll->server;
    return autoscroll->enabled && autoscroll->surface && !server->locked
        && server->seat->pointer_state.focused_surface == autoscroll->surface
        && vela_views_focused(server) == autoscroll->view && !server->focused_layer
        && server->cursor_mode == VELA_CURSOR_PASSTHROUGH && !server->seat->drag
        && !server->input->active_constraint && !vela_input_shortcuts_inhibited(server->input)
        && !vela_switcher_active(server)
        && output_alive(server, autoscroll->output);
}

static void scroll(struct vela_autoscroll *autoscroll, double seconds)
{
    struct vela_server *server = autoscroll->server;
    struct wlr_cursor *cursor = server->cursor;
    double offsets[2] = { cursor->y - autoscroll->anchor_y, cursor->x - autoscroll->anchor_x };
    uint32_t ms = (uint32_t)(vela_now_ns() / VELA_NS_PER_MS);
    bool any = false;
    for (int axis = 0; axis < 2; ++axis) {
        double speed = vela_autoscroll_speed(offsets[axis], autoscroll->speed);
        if (speed == 0.0) {
            autoscroll->carry[axis] = 0.0;
            continue;
        }
        // The protocol carries 1/256 px: what doesn't fit goes to the next
        // frame, so slow speeds aren't lost or uneven.
        double amount = speed * seconds + autoscroll->carry[axis];
        double sent = wl_fixed_to_double(wl_fixed_from_double(amount));
        autoscroll->carry[axis] = amount - sent;
        if (sent == 0.0) {
            continue;
        }
        wlr_seat_pointer_notify_axis(server->seat, ms,
            axis ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL, sent, 0,
            WL_POINTER_AXIS_SOURCE_CONTINUOUS, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
        autoscroll->scrolled[axis] = true;
        any = true;
    }
    if (any) {
        wlr_seat_pointer_notify_frame(server->seat);
    }
}

static int handle_tick(int fd, uint32_t mask, void *data)
{
    struct vela_autoscroll *autoscroll = data;
    uint64_t expirations = 0;
    if (read(fd, &expirations, sizeof(expirations)) < 0) {
        return 0;
    }
    if (autoscroll->pending) {
        // Held long enough: a held autoscroll (or nothing, if things changed).
        if (still_valid(autoscroll)) {
            activate(autoscroll);
        } else {
            cancel_pending(autoscroll, false);
        }
        return 0;
    }
    if (!autoscroll->active) {
        return 0;
    }
    if (!still_valid(autoscroll)) {
        stop(autoscroll);
        return 0;
    }
    // The time between two vblanks of the grid, not between two wake-ups:
    // the same step every frame even when the timer wakes a little late.
    int64_t tick = autoscroll->next_tick;
    int64_t step = tick - autoscroll->last_tick;
    step = step < MAX_STEP_NS ? step : MAX_STEP_NS;
    autoscroll->last_tick = tick;
    if (step > 0) {
        scroll(autoscroll, step / 1e9);
    }
    autoscroll->next_tick = next_vblank(autoscroll, vela_now_ns());
    arm(autoscroll, autoscroll->next_tick);
    return 0;
}

static void handle_surface_destroy(struct wl_listener *listener, void *data)
{
    struct vela_autoscroll *autoscroll = wl_container_of(listener, autoscroll, surface_destroy);
    // Not from inside the surface's destruction: at the next tick, now.
    vela_unlisten(&autoscroll->surface_destroy);
    autoscroll->surface = NULL;
    arm(autoscroll, vela_now_ns());
}

// The theme's autoscroll cursor; older themes only have the X11 names.
static const char *scroll_cursor(struct vela_server *server)
{
    const char *names[] = { "all-scroll", "fleur", "move" };
    if (wlr_xcursor_manager_load(server->cursor_manager, 1.0f)) {
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (wlr_xcursor_manager_get_xcursor(server->cursor_manager, names[i], 1.0f)) {
                return names[i];
            }
        }
    }
    return "default";
}

static struct vela_view *view_of(void *owner)
{
    struct vela_owner *o = owner;
    return o && o->kind == VELA_OWNER_VIEW ? (struct vela_view *)o : NULL;
}

static void activate(struct vela_autoscroll *autoscroll)
{
    struct vela_server *server = autoscroll->server;
    autoscroll->pending = false;
    autoscroll->active = true;
    autoscroll->held = true;
    autoscroll->carry[0] = autoscroll->carry[1] = 0.0;
    autoscroll->scrolled[0] = autoscroll->scrolled[1] = false;
    wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, scroll_cursor(server));
    // The app gets no more motion from here: where it is now.
    autoscroll->last_tick = vela_now_ns();
    autoscroll->next_tick = next_vblank(autoscroll, autoscroll->last_tick);
    arm(autoscroll, autoscroll->next_tick);
    wlr_log(WLR_DEBUG, "Autoscroll: on (%s) at %.0f,%.0f", vela_view_app_id(autoscroll->view), autoscroll->anchor_x,
        autoscroll->anchor_y);
}

// The held-back press after all: to the app (with its release if `click`),
// or dropped if the window is gone.
static void cancel_pending(struct vela_autoscroll *autoscroll, bool click)
{
    struct vela_server *server = autoscroll->server;
    struct wlr_seat *seat = server->seat;
    autoscroll->pending = false;
    disarm(autoscroll);
    vela_unlisten(&autoscroll->surface_destroy);
    if (autoscroll->surface && seat->pointer_state.focused_surface == autoscroll->surface) {
        wlr_seat_pointer_notify_button(seat, autoscroll->pending_time, BTN_MIDDLE, WL_POINTER_BUTTON_STATE_PRESSED);
        if (click) {
            uint32_t ms = (uint32_t)(vela_now_ns() / VELA_NS_PER_MS);
            wlr_seat_pointer_notify_button(seat, ms, BTN_MIDDLE, WL_POINTER_BUTTON_STATE_RELEASED);
        } else {
            swallowed_remove(autoscroll, BTN_MIDDLE); // its release goes to the app as usual
        }
        wlr_seat_pointer_notify_frame(seat);
    }
    autoscroll->surface = NULL;
    autoscroll->view = NULL;
    autoscroll->output = NULL;
}

// The middle button pressed: starts if the pointer is on an app's window
// (not the title bar, borders or shell) and nothing else is going on.
static bool start(struct vela_autoscroll *autoscroll, uint32_t time_msec)
{
    struct vela_server *server = autoscroll->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_cursor *cursor = server->cursor;
    struct vela_interaction *interaction = server->interaction;
    if (!autoscroll->enabled || !(autoscroll->click || autoscroll->hold) || !autoscroll->timer
        || server->locked || server->cursor_mode != VELA_CURSOR_PASSTHROUGH || seat->drag
        || seat->pointer_state.button_count > 0 || server->input->active_constraint
        || vela_input_shortcuts_inhibited(server->input) || interaction->keyboard.view
        || interaction->pending_title_drag.view || vela_switcher_active(server)) {
        return false;
    }
    struct vela_hit hit = vela_scene_at(server->scene, cursor->x, cursor->y);
    struct vela_view *view = view_of(hit.owner);
    struct vela_output *output = vela_output_at(server, cursor->x, cursor->y);
    if (!view || !hit.surface || hit.surface != seat->pointer_state.focused_surface || !output
        || !allowed_for(view)) {
        return false;
    }
    // Like any click, it activates the window; not under a system dialog.
    vela_focus_view(server, view);
    if (vela_views_focused(server) != view || server->focused_layer) {
        return false;
    }
    autoscroll->moved = false;
    autoscroll->pressed_at = vela_now_ns();
    autoscroll->pending_time = time_msec;
    autoscroll->anchor_x = cursor->x;
    autoscroll->anchor_y = cursor->y;
    autoscroll->surface = hit.surface;
    autoscroll->view = view;
    autoscroll->output = output;
    vela_listen(&hit.surface->events.destroy, &autoscroll->surface_destroy, handle_surface_destroy);
    if (autoscroll->click) {
        activate(autoscroll);
    } else {
        autoscroll->pending = true;
        arm(autoscroll, autoscroll->pressed_at + HOLD_MS * VELA_NS_PER_MS);
    }
    return true;
}

// ---------------------------------------------------------------- events --

bool vela_autoscroll_active(const struct vela_autoscroll *autoscroll)
{
    return autoscroll && autoscroll->active;
}


bool vela_autoscroll_button(struct vela_autoscroll *autoscroll, const struct wlr_pointer_button_event *event)
{
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (!swallowed_take(autoscroll, event->button)) {
            return false;
        }
        if (autoscroll->pending && event->button == BTN_MIDDLE) {
            cancel_pending(autoscroll, true); // a click: the app's
        } else if (autoscroll->active && autoscroll->held && event->button == BTN_MIDDLE) {
            autoscroll->held = false;
            // Dragged (or held a long time) it ends with the release; without
            // autoscroll-click, always.
            bool dragged
                = autoscroll->moved || vela_now_ns() - autoscroll->pressed_at > CLICK_MS * VELA_NS_PER_MS;
            if (!autoscroll->click || (autoscroll->hold && dragged)) {
                stop(autoscroll);
            }
        }
        return true;
    }
    if (autoscroll->pending) {
        // Another button: the middle one was just a press, for the app.
        cancel_pending(autoscroll, false);
        return false;
    }
    if (autoscroll->active) {
        // Any button ends it. A non-Linux button code cannot be tracked:
        // forward that complete click instead of leaking only its release.
        stop(autoscroll);
        if (!valid_button(event->button)) {
            return false;
        }
        swallowed_add(autoscroll, event->button);
        return true;
    }
    if (event->button == BTN_MIDDLE && start(autoscroll, event->time_msec)) {
        swallowed_add(autoscroll, event->button);
        return true;
    }
    return false;
}

bool vela_autoscroll_motion(struct vela_autoscroll *autoscroll)
{
    struct wlr_cursor *cursor = autoscroll->server->cursor;
    bool far = hypot(cursor->x - autoscroll->anchor_x, cursor->y - autoscroll->anchor_y) > CLICK_DISTANCE;
    if (autoscroll->pending && far) {
        autoscroll->moved = true;
        if (still_valid(autoscroll)) {
            activate(autoscroll);
        } else {
            cancel_pending(autoscroll, false);
        }
    } else if (autoscroll->active && autoscroll->held && far) {
        autoscroll->moved = true;
    }
    return autoscroll->active;
}

bool vela_autoscroll_key(struct vela_autoscroll *autoscroll, const uint32_t *syms, int count, bool pressed)
{
    for (int i = 0; i < count; ++i) {
        if (syms[i] != XKB_KEY_Escape) {
            continue;
        }
        // An intercepted key-down must never leave an orphan key-up in the
        // focused app, even though stopping deactivates autoscroll.
        if (!pressed && autoscroll->escape_swallowed) {
            autoscroll->escape_swallowed = false;
            return true;
        }
        if (pressed && autoscroll->active) {
            autoscroll->escape_swallowed = true;
            stop(autoscroll);
            return true;
        }
    }
    return false;
}

// -------------------------------------------------------------- creation --

struct vela_autoscroll *vela_autoscroll_create(struct vela_server *server)
{
    struct vela_autoscroll *autoscroll = calloc(1, sizeof(*autoscroll));
    autoscroll->server = server;
    wl_list_init(&autoscroll->surface_destroy.link);
    autoscroll->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (autoscroll->timer_fd >= 0) {
        autoscroll->timer
            = wl_event_loop_add_fd(server->loop, autoscroll->timer_fd, WL_EVENT_READABLE, handle_tick, autoscroll);
    } else {
        wlr_log_errno(WLR_ERROR, "Autoscroll: timerfd");
    }
    vela_autoscroll_load_settings(autoscroll);
    return autoscroll;
}

void vela_autoscroll_destroy(struct vela_autoscroll *autoscroll)
{
    if (!autoscroll) {
        return;
    }
    vela_unlisten(&autoscroll->surface_destroy);
    if (autoscroll->timer) {
        wl_event_source_remove(autoscroll->timer);
    }
    if (autoscroll->timer_fd >= 0) {
        close(autoscroll->timer_fd);
    }
    free(autoscroll);
}
