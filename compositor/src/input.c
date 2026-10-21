// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "input.h"

#include "autoscroll.h"
#include "listen.h"
#include "workspace.h"
#include "view.h"
#include "switcher.h"
#include "lock.h"
#include "interact.h"
#include "config.h"
#include "keyboard.h"
#include "output.h"
#include "nested.h"
#include "scene/scene.h"
#include "server.h"
#include "shell.h"
#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/config.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <xkbcommon/xkbcommon.h>

#if WLR_HAS_LIBINPUT_BACKEND
#include <libinput.h>
#include <wlr/backend/libinput.h>
#endif

// How far to move (touchpad units) for a gesture to count.
#define SWIPE_THRESHOLD 90.0
// "Switch app": one step per this much movement.
#define APP_STEP 140.0

// A mouse or a touchpad.
struct vela_pointer {
    struct wl_list link; // vela_input.pointers
    struct vela_input *input;
    struct wlr_input_device *device;
    bool touchpad;
    struct wl_listener destroy;
};

// A live constraint: when it goes away it must not stay the active one.
struct vela_constraint {
    struct vela_input *input;
    struct wlr_pointer_constraint_v1 *wlr;
    struct wl_listener destroy;
};

// -------------------------------------------------------------- settings --

static enum vela_swipe_action swipe_action(const char *name)
{
    if (strcmp(name, "app") == 0) {
        return VELA_SWIPE_APP;
    }
    if (strcmp(name, "desktop") == 0) {
        return VELA_SWIPE_DESKTOP;
    }
    return strcmp(name, "no") == 0 ? VELA_SWIPE_NONE : VELA_SWIPE_OTHER;
}

#if WLR_HAS_LIBINPUT_BACKEND
// Like the Windows pointer: 1-20, 10 in the middle; libinput goes from -1 to
// 1.
static double speed_setting(const struct vela_config *settings, const char *key)
{
    int value = vela_clamp(atoi(vela_config_get(settings, key, "10")), 1, 20);
    return (value - 10) / 10.0;
}

static bool mouse_present(const struct vela_input *input)
{
    const struct vela_pointer *pointer;
    wl_list_for_each (pointer, &input->pointers, link) {
        if (!pointer->touchpad && wlr_input_device_is_libinput(pointer->device)) {
            return true;
        }
    }
    return false;
}
#endif

static void configure_pointer(struct vela_pointer *pointer, const struct vela_config *settings)
{
#if WLR_HAS_LIBINPUT_BACKEND
    if (!wlr_input_device_is_libinput(pointer->device)) {
        return; // such as the nested backend: the pointer belongs to the host system
    }
    struct libinput_device *handle = wlr_libinput_get_device_handle(pointer->device);

    // The primary button (for the touchpad too, like Windows).
    if (libinput_device_config_left_handed_is_available(handle)) {
        libinput_device_config_left_handed_set(handle,
            strcmp(vela_config_get(settings, "mouse-primary-button", ""), "right") == 0);
    }
    if (!pointer->touchpad) {
        if (libinput_device_config_accel_is_available(handle)) {
            libinput_device_config_accel_set_speed(handle, speed_setting(settings, "mouse-speed"));
            libinput_device_config_accel_set_profile(handle, vela_config_flag(settings, "mouse-precision", true)
                    ? LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE
                    : LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
        }
        return;
    }

    // Touchpad: off, or off only while there is also a mouse.
    bool enabled = vela_config_flag(settings, "touchpad", true);
    bool with_mouse = vela_config_flag(settings, "touchpad-with-mouse", true);
    libinput_device_config_send_events_set_mode(handle,
        !enabled || (!with_mouse && mouse_present(pointer->input)) ? LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
                                                                   : LIBINPUT_CONFIG_SEND_EVENTS_ENABLED);
    if (libinput_device_config_accel_is_available(handle)) {
        libinput_device_config_accel_set_speed(handle, speed_setting(settings, "touchpad-speed"));
    }
    bool tap = vela_config_flag(settings, "touchpad-tap", true);
    libinput_device_config_tap_set_enabled(handle, tap ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
    libinput_device_config_tap_set_drag_enabled(handle,
        tap ? LIBINPUT_CONFIG_DRAG_ENABLED : LIBINPUT_CONFIG_DRAG_DISABLED);
    // Two fingers: right button, three: middle (like Windows).
    libinput_device_config_tap_set_button_map(handle, LIBINPUT_CONFIG_TAP_MAP_LRM);
    if (libinput_device_config_dwt_is_available(handle)) {
        libinput_device_config_dwt_set_enabled(handle, LIBINPUT_CONFIG_DWT_ENABLED);
    }
    if (libinput_device_config_scroll_has_natural_scroll(handle)) {
        // "Down motion scrolls up", like Windows.
        bool natural = vela_config_flag(settings, "touchpad-natural-scroll", true);
        const char *env = getenv("VELA_NATURAL_SCROLL");
        if (env && *env) {
            natural = strcmp(env, "0") != 0;
        }
        libinput_device_config_scroll_set_natural_scroll_enabled(handle, natural);
    }
    // Touchpad buttons: a two-finger click is the right button (clickfinger),
    // where the touchpad has no painted zones.
    if (libinput_device_config_click_get_methods(handle) & LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER) {
        libinput_device_config_click_set_method(handle, LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER);
    }
    wlr_log(WLR_INFO, "Touchpad configured: %s%s", libinput_device_get_name(handle),
        libinput_device_config_send_events_get_mode(handle) == LIBINPUT_CONFIG_SEND_EVENTS_DISABLED ? " (off)" : "");
#else
    (void)pointer;
    (void)settings;
#endif
}

// Mouse and touchpad from vela.conf, to all devices.
static void load_pointer_settings(struct vela_input *input)
{
    struct vela_config settings;
    vela_config_read(&settings);
    input->wheel_factor = vela_clamp(atoi(vela_config_get(&settings, "mouse-scroll-lines", "3")), 1, 20) / 3.0;
    input->three_fingers = swipe_action(vela_config_get(&settings, "touchpad-three-fingers", "app"));
    input->four_fingers = swipe_action(vela_config_get(&settings, "touchpad-four-fingers", "desktop"));
    struct vela_pointer *pointer;
    wl_list_for_each (pointer, &input->pointers, link) {
        configure_pointer(pointer, &settings);
    }
    vela_config_finish(&settings);
}

void vela_input_reload(struct vela_input *input)
{
    struct vela_config settings;
    vela_config_read(&settings);
    struct vela_keyboard *keyboard;
    wl_list_for_each (keyboard, &input->keyboards, link) {
        vela_keyboard_apply_settings(keyboard, &settings);
    }
    vela_config_finish(&settings);
    struct wlr_seat *seat = input->server->seat;
    if (seat->keyboard_state.keyboard) {
        wlr_seat_set_keyboard(seat, seat->keyboard_state.keyboard); // the new layout to the apps
    }
    load_pointer_settings(input);
}

bool vela_input_has_touchpad(const struct vela_input *input)
{
    const struct vela_pointer *pointer;
    wl_list_for_each (pointer, &input->pointers, link) {
        if (pointer->touchpad) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- devices --

static void pointer_destroy(struct vela_pointer *pointer)
{
    wl_list_remove(&pointer->link);
    wl_list_remove(&pointer->destroy.link);
    free(pointer);
}

static void handle_pointer_destroy(struct wl_listener *listener, void *data)
{
    struct vela_pointer *pointer = wl_container_of(listener, pointer, destroy);
    struct vela_input *input = pointer->input;
    pointer_destroy(pointer);
    load_pointer_settings(input); // one mouse less: the touchpad may turn back on
}

static void add_pointer(struct vela_input *input, struct wlr_input_device *device)
{
    struct vela_pointer *pointer = calloc(1, sizeof(*pointer));
    pointer->input = input;
    pointer->device = device;
#if WLR_HAS_LIBINPUT_BACKEND
    if (wlr_input_device_is_libinput(device)) {
        pointer->touchpad = libinput_device_config_tap_get_finger_count(wlr_libinput_get_device_handle(device)) > 0;
    }
#endif
    vela_listen(&device->events.destroy, &pointer->destroy, handle_pointer_destroy);
    wl_list_insert(input->pointers.prev, &pointer->link);
    load_pointer_settings(input);
}

static void add_device(struct vela_input *input, struct wlr_input_device *device)
{
    struct vela_server *server = input->server;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        vela_keyboard_create(input, wlr_keyboard_from_input_device(device));
        break;
    case WLR_INPUT_DEVICE_POINTER:
        add_pointer(input, device);
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    default:
        break;
    }
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&input->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void handle_new_input(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, new_input);
    add_device(input, data);
}

static void handle_new_virtual_pointer(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, new_virtual_pointer);
    struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
    add_device(input, &event->new_pointer->pointer.base);
}

static void handle_new_virtual_keyboard(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *keyboard = data;
    add_device(input, &keyboard->keyboard.base);
}

// --------------------------------------------------- pointer constraints --

static void handle_constraint_destroy(struct wl_listener *listener, void *data)
{
    struct vela_constraint *constraint = wl_container_of(listener, constraint, destroy);
    if (constraint->input->active_constraint == constraint->wlr) {
        constraint->input->active_constraint = NULL;
    }
    wl_list_remove(&constraint->destroy.link);
    free(constraint);
}

static void handle_new_constraint(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, new_constraint);
    struct wlr_pointer_constraint_v1 *wlr = data;
    struct vela_constraint *constraint = calloc(1, sizeof(*constraint));
    constraint->input = input;
    constraint->wlr = wlr;
    vela_listen(&wlr->events.destroy, &constraint->destroy, handle_constraint_destroy);
    // The pointer is already on the surface: it applies at once.
    if (input->server->seat->pointer_state.focused_surface == wlr->surface) {
        vela_input_constrain(input, wlr->surface);
    }
}

void vela_input_constrain(struct vela_input *input, struct wlr_surface *surface)
{
    struct wlr_seat *seat = input->server->seat;
    struct wlr_cursor *cursor = input->server->cursor;
    struct wlr_pointer_constraint_v1 *constraint
        = surface ? wlr_pointer_constraints_v1_constraint_for_surface(input->pointer_constraints, surface, seat) : NULL;
    if (constraint == input->active_constraint) {
        return;
    }
    struct wlr_pointer_constraint_v1 *previous = input->active_constraint;
    if (previous) {
        // Unlocked: the cursor shows again where the app says it left it.
        if (previous->type == WLR_POINTER_CONSTRAINT_V1_LOCKED && previous->current.cursor_hint.enabled
            && seat->pointer_state.focused_surface == previous->surface) {
            double origin_x = cursor->x - seat->pointer_state.sx;
            double origin_y = cursor->y - seat->pointer_state.sy;
            wlr_cursor_warp(cursor, NULL, origin_x + previous->current.cursor_hint.x,
                origin_y + previous->current.cursor_hint.y);
        }
        input->active_constraint = NULL;
        wlr_pointer_constraint_v1_send_deactivated(previous); // can destroy it
    }
    if (constraint) {
        input->active_constraint = constraint;
        wlr_pointer_constraint_v1_send_activated(constraint);
    }
}

// Relative mouse motion: false if the pointer is locked; if confined, the
// motion stops at the region's edge.
static bool constrain_motion(struct vela_input *input, double *dx, double *dy)
{
    struct wlr_seat *seat = input->server->seat;
    struct wlr_pointer_constraint_v1 *constraint = input->active_constraint;
    if (!constraint || input->server->cursor_mode != VELA_CURSOR_PASSTHROUGH
        || seat->pointer_state.focused_surface != constraint->surface) {
        return true;
    }
    if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        return false;
    }
    // Surface coordinates.
    double sx = seat->pointer_state.sx;
    double sy = seat->pointer_state.sy;
    double x = sx + *dx;
    double y = sy + *dy;
    if (wlr_region_confine(&constraint->region, sx, sy, sx + *dx, sy + *dy, &x, &y)) {
        *dx = x - sx;
        *dy = y - sy;
    }
    return true;
}

static void handle_new_inhibitor(struct wl_listener *listener, void *data)
{
    // Always granted: apps the user runs fullscreen ask for it (virtual
    // machines, remote desktop). It applies only while they are focused.
    wlr_keyboard_shortcuts_inhibitor_v1_activate(data);
}

bool vela_input_shortcuts_inhibited(const struct vela_input *input)
{
    struct wlr_surface *focused = input->server->seat->keyboard_state.focused_surface;
    if (!focused || !input->shortcuts_inhibit) {
        return false;
    }
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    wl_list_for_each (inhibitor, &input->shortcuts_inhibit->inhibitors, link) {
        if (inhibitor->surface == focused && inhibitor->active) {
            return true;
        }
    }
    return false;
}

// --------------------------------------------------------------- pointer --

static void handle_motion(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, motion);
    struct vela_server *server = input->server;
    struct wlr_pointer_motion_event *event = data;
    vela_lock_note_activity(server->lock);
    // Check both sides of the motion: an autoscroll pending on a held middle
    // press can activate on THIS motion. Neither it nor later motion may
    // reach relative-pointer clients while autoscrolling.
    bool was_autoscrolling = vela_autoscroll_active(server->autoscroll);
    double dx = event->delta_x;
    double dy = event->delta_y;
    if (constrain_motion(input, &dx, &dy)) {
        wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
        vela_interact_motion(server, event->time_msec);
    }
    // Games still get raw deltas when the pointer is locked, except while
    // autoscroll owns the movement.
    if (!was_autoscrolling && !vela_autoscroll_active(server->autoscroll)) {
        wlr_relative_pointer_manager_v1_send_relative_motion(input->relative_pointers, server->seat,
            (uint64_t)event->time_msec * 1000, event->delta_x, event->delta_y, event->unaccel_dx, event->unaccel_dy);
    }
}

static void handle_motion_absolute(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, motion_absolute);
    struct vela_server *server = input->server;
    struct wlr_pointer_motion_absolute_event *event = data;
    vela_lock_note_activity(server->lock);
    double x = event->x;
    double y = event->y;
    // Nested: the backend divides by the buffer pixels, which with a host at a
    // fractional scale are more than the window's units.
    if (wlr_input_device_is_wl(&event->pointer->base)) {
        struct vela_output *out = vela_output_named(server, event->pointer->output_name);
        if (out && out->nested) {
            x *= vela_nested_pointer_scale_x(out->nested);
            y *= vela_nested_pointer_scale_y(out->nested);
        }
    }
    // Absolute motion too (tablets, nested Vela) respects a pointer locked or
    // confined by the app.
    double lx = 0.0;
    double ly = 0.0;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->pointer->base, x, y, &lx, &ly);
    double dx = lx - server->cursor->x;
    double dy = ly - server->cursor->y;
    if (!constrain_motion(input, &dx, &dy)) {
        return;
    }
    wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
    vela_interact_motion(server, event->time_msec);
}

static void handle_button(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, button);
    vela_interact_button(input->server, data);
}

static void handle_axis(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, axis);
    struct wlr_pointer_axis_event *event = data;
    vela_lock_note_activity(input->server->lock);
    // The mouse wheel: how many lines per notch (Settings > Mouse).
    double lines = event->source == WL_POINTER_AXIS_SOURCE_WHEEL ? input->wheel_factor : 1.0;
    wlr_seat_pointer_notify_axis(input->server->seat, event->time_msec, event->orientation, event->delta * lines,
        (int32_t)lround(event->delta_discrete * lines), event->source, event->relative_direction);
}

static void handle_frame(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, frame);
    wlr_seat_pointer_notify_frame(input->server->seat);
}

// --------------------------------------------------------------- gestures --

static void handle_swipe_begin(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, swipe_begin);
    struct vela_server *server = input->server;
    struct wlr_pointer_swipe_begin_event *event = data;
    vela_lock_note_activity(server->lock);
    memset(&input->swipe, 0, sizeof(input->swipe));
    input->swipe.action = event->fingers == 3 ? input->three_fingers
        : event->fingers == 4                 ? input->four_fingers
                                              : VELA_SWIPE_NONE;
    input->swipe.ours = input->swipe.action != VELA_SWIPE_NONE && !server->locked
        && !vela_input_shortcuts_inhibited(input);
    if (!input->swipe.ours) {
        wlr_pointer_gestures_v1_send_swipe_begin(input->gestures, server->seat, event->time_msec, event->fingers);
    }
}

static void handle_swipe_update(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, swipe_update);
    struct wlr_pointer_swipe_update_event *event = data;
    if (!input->swipe.ours) {
        wlr_pointer_gestures_v1_send_swipe_update(input->gestures, input->server->seat, event->time_msec, event->dx,
            event->dy);
        return;
    }
    input->swipe.dx += event->dx;
    input->swipe.dy += event->dy;
    // "Switch app": the Alt+Tab panel follows the fingers.
    if (input->swipe.action == VELA_SWIPE_APP && fabs(input->swipe.dx) > fabs(input->swipe.dy)) {
        int steps = (int)(input->swipe.dx / APP_STEP);
        while (input->swipe.steps != steps) {
            int direction = steps > input->swipe.steps ? 1 : -1;
            vela_switcher_step(input->server, direction);
            input->swipe.steps += direction;
        }
    }
}

static void handle_swipe_end(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, swipe_end);
    struct vela_server *server = input->server;
    struct wlr_pointer_swipe_end_event *event = data;
    if (!input->swipe.ours) {
        wlr_pointer_gestures_v1_send_swipe_end(input->gestures, server->seat, event->time_msec, event->cancelled);
        return;
    }
    if (vela_switcher_active(server)) {
        vela_switcher_finish(server, !event->cancelled);
        return;
    }
    if (event->cancelled) {
        return;
    }
    double dx = input->swipe.dx;
    double dy = input->swipe.dy;
    bool vertical = fabs(dy) > fabs(dx);
    if (vertical && dy < -SWIPE_THRESHOLD) {
        vela_shell_send(server, "task-view"); // up
    } else if (vertical && dy > SWIPE_THRESHOLD) {
        vela_shell_send(server, "show-desktop"); // down
    } else if (!vertical && input->swipe.action == VELA_SWIPE_DESKTOP && fabs(dx) > SWIPE_THRESHOLD) {
        // Fingers moving left bring the desktop on the right, like Windows.
        vela_workspaces_switch(server, server->workspaces->current + (dx < 0 ? 1 : -1), true);
    }
}

// Pinch (zoom in apps) and hold: always to the apps.
static void handle_pinch_begin(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, pinch_begin);
    struct wlr_pointer_pinch_begin_event *event = data;
    vela_lock_note_activity(input->server->lock);
    wlr_pointer_gestures_v1_send_pinch_begin(input->gestures, input->server->seat, event->time_msec, event->fingers);
}

static void handle_pinch_update(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, pinch_update);
    struct wlr_pointer_pinch_update_event *event = data;
    wlr_pointer_gestures_v1_send_pinch_update(input->gestures, input->server->seat, event->time_msec, event->dx,
        event->dy, event->scale, event->rotation);
}

static void handle_pinch_end(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, pinch_end);
    struct wlr_pointer_pinch_end_event *event = data;
    wlr_pointer_gestures_v1_send_pinch_end(input->gestures, input->server->seat, event->time_msec, event->cancelled);
}

static void handle_hold_begin(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, hold_begin);
    struct wlr_pointer_hold_begin_event *event = data;
    wlr_pointer_gestures_v1_send_hold_begin(input->gestures, input->server->seat, event->time_msec, event->fingers);
}

static void handle_hold_end(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, hold_end);
    struct wlr_pointer_hold_end_event *event = data;
    wlr_pointer_gestures_v1_send_hold_end(input->gestures, input->server->seat, event->time_msec, event->cancelled);
}

// ------------------------------------------------ seat: cursor, clipboard --

static void handle_request_set_cursor(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, request_set_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    if (input->server->seat->pointer_state.focused_client == event->seat_client
        && !vela_autoscroll_active(input->server->autoscroll)) {
        wlr_cursor_set_surface(input->server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

// Recent Qt/GTK apps ask for the cursor shape by name instead of drawing it
// themselves: faster and consistent with the theme.
static void handle_request_set_shape(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, request_set_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    struct vela_server *server = input->server;
    if (event->seat_client == server->seat->pointer_state.focused_client
        && !vela_autoscroll_active(server->autoscroll)) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, wlr_cursor_shape_v1_name(event->shape));
    }
}

static void handle_request_set_selection(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(input->server->seat, event->source, event->serial);
}

static void handle_request_set_primary_selection(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(input->server->seat, event->source, event->serial);
}

// Dragging between apps: accepted only from whoever really holds the button on
// its own surface (the click's serial).
static void handle_request_start_drag(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, request_start_drag);
    struct wlr_seat_request_start_drag_event *event = data;
    struct wlr_seat *seat = input->server->seat;
    if (wlr_seat_validate_pointer_grab_serial(seat, event->origin, event->serial)) {
        wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
    } else if (event->drag->source) {
        wlr_data_source_destroy(event->drag->source);
    }
}

static void drag_icon_destroy(struct vela_drag_icon *icon)
{
    if (!icon) {
        return;
    }
    wl_list_remove(&icon->commit.link);
    wl_list_remove(&icon->destroy.link);
    vela_node_destroy(&icon->tree->node);
    icon->input->drag_icon = NULL;
    free(icon);
}

void vela_input_keyboard_enter(struct vela_input *input, struct wlr_surface *surface)
{
    struct wlr_seat *seat = input->server->seat;
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard) {
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes,
            &keyboard->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
    }
}

void vela_input_update_drag_icon(struct vela_input *input)
{
    struct vela_drag_icon *icon = input->drag_icon;
    if (icon) {
        struct wlr_cursor *cursor = input->server->cursor;
        vela_node_set_position(&icon->tree->node, cursor->x + icon->dx, cursor->y + icon->dy);
    }
}

static void handle_drag_icon_commit(struct wl_listener *listener, void *data)
{
    struct vela_drag_icon *icon = wl_container_of(listener, icon, commit);
    icon->dx += icon->surface->current.dx;
    icon->dy += icon->surface->current.dy;
    vela_input_update_drag_icon(icon->input);
}

static void handle_drag_icon_destroy(struct wl_listener *listener, void *data)
{
    struct vela_drag_icon *icon = wl_container_of(listener, icon, destroy);
    drag_icon_destroy(icon);
}

static void handle_start_drag(struct wl_listener *listener, void *data)
{
    struct vela_input *input = wl_container_of(listener, input, start_drag);
    struct wlr_drag *drag = data;
    // From here the drag drives the pointer.
    memset(&input->implicit_grab, 0, sizeof(input->implicit_grab));
    if (!drag->icon) {
        return;
    }
    drag_icon_destroy(input->drag_icon); // one drag at a time
    struct wlr_surface *surface = drag->icon->surface;
    struct vela_drag_icon *icon = calloc(1, sizeof(*icon));
    icon->input = input;
    icon->surface = surface;
    icon->tree = vela_tree_create(input->server->layers.drag);
    icon->node = vela_surface_node_create(icon->tree, surface);
    vela_listen(&surface->events.commit, &icon->commit, handle_drag_icon_commit);
    vela_listen(&drag->icon->events.destroy, &icon->destroy, handle_drag_icon_destroy);
    input->drag_icon = icon;
    vela_input_update_drag_icon(input);
}

// --------------------------------------------------------------- pasting --

// The key that types "v" in the current layout (0: none).
static xkb_keycode_t key_for_v(struct xkb_keymap *keymap)
{
    for (xkb_keycode_t code = xkb_keymap_min_keycode(keymap); code <= xkb_keymap_max_keycode(keymap); ++code) {
        const xkb_keysym_t *syms = NULL;
        int n = xkb_keymap_key_get_syms_by_level(keymap, code, 0, 0, &syms);
        for (int i = 0; i < n; ++i) {
            if (syms[i] == XKB_KEY_v) {
                return code;
            }
        }
    }
    return 0;
}

// Terminals paste with Ctrl+Shift+V.
static bool is_terminal(const char *app)
{
    const char *names[] = { "konsole", "terminal", "kitty", "alacritty", "foot", "wezterm", "ghostty" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strstr(app, names[i])) {
            return true;
        }
    }
    return false;
}

static int handle_paste_timer(void *data)
{
    struct vela_input *input = data;
    struct vela_server *server = input->server;
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    if (!keyboard && !wl_list_empty(&input->keyboards)) {
        struct vela_keyboard *first = wl_container_of(input->keyboards.next, first, link);
        keyboard = first->wlr;
        wlr_seat_set_keyboard(server->seat, keyboard);
    }
    // The active window's app ("" if it has no app id: there is one, but it's
    // not a terminal).
    struct vela_view *focused = vela_views_focused(server);
    const char *app = focused ? vela_view_app_id(focused) : NULL;
    if (!keyboard || !keyboard->keymap || !app || server->locked) {
        return 0;
    }
    struct xkb_keymap *keymap = keyboard->keymap;
    xkb_keycode_t v = key_for_v(keymap);
    if (!v) {
        return 0;
    }
    uint32_t mods = 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
    if (is_terminal(app)) {
        mods |= 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
    }
    uint32_t ms = (uint32_t)(vela_now_ns() / VELA_NS_PER_MS);
    struct wlr_keyboard_modifiers pressed = keyboard->modifiers;
    pressed.depressed |= mods;
    wlr_seat_keyboard_notify_modifiers(server->seat, &pressed);
    wlr_seat_keyboard_notify_key(server->seat, ms, v - 8, WL_KEYBOARD_KEY_STATE_PRESSED);
    wlr_seat_keyboard_notify_key(server->seat, ms, v - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
    wlr_seat_keyboard_notify_modifiers(server->seat, &keyboard->modifiers);
    return 0;
}

void vela_input_paste(struct vela_input *input)
{
    // A moment later: the shell panel closes and the keyboard goes back to the
    // app, then Ctrl+V.
    if (!input->paste_timer) {
        input->paste_timer = wl_event_loop_add_timer(input->server->loop, handle_paste_timer, input);
    }
    wl_event_source_timer_update(input->paste_timer, 120);
}

// -------------------------------------------------------------- creation --

struct vela_input *vela_input_create(struct vela_server *server)
{
    struct vela_input *input = calloc(1, sizeof(*input));
    input->server = server;
    wl_list_init(&input->pointers);
    wl_list_init(&input->keyboards);
    input->wheel_factor = 1.0;
    input->three_fingers = VELA_SWIPE_APP;
    input->four_fingers = VELA_SWIPE_DESKTOP;
    struct wl_display *display = server->display;

    server->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    server->cursor_manager
        = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), (uint32_t)vela_env_int("XCURSOR_SIZE", 24));

    // Games and apps that want the mouse to themselves.
    input->relative_pointers = wlr_relative_pointer_manager_v1_create(display);
    input->pointer_constraints = wlr_pointer_constraints_v1_create(display);
    vela_listen(&input->pointer_constraints->events.new_constraint, &input->new_constraint, handle_new_constraint);
    input->shortcuts_inhibit = wlr_keyboard_shortcuts_inhibit_v1_create(display);
    vela_listen(&input->shortcuts_inhibit->events.new_inhibitor, &input->new_inhibitor, handle_new_inhibitor);

    struct wlr_cursor *cursor = server->cursor;
    vela_listen(&cursor->events.motion, &input->motion, handle_motion);
    vela_listen(&cursor->events.motion_absolute, &input->motion_absolute, handle_motion_absolute);
    vela_listen(&cursor->events.button, &input->button, handle_button);
    vela_listen(&cursor->events.axis, &input->axis, handle_axis);
    input->gestures = wlr_pointer_gestures_v1_create(display);
    vela_listen(&cursor->events.swipe_begin, &input->swipe_begin, handle_swipe_begin);
    vela_listen(&cursor->events.swipe_update, &input->swipe_update, handle_swipe_update);
    vela_listen(&cursor->events.swipe_end, &input->swipe_end, handle_swipe_end);
    vela_listen(&cursor->events.pinch_begin, &input->pinch_begin, handle_pinch_begin);
    vela_listen(&cursor->events.pinch_update, &input->pinch_update, handle_pinch_update);
    vela_listen(&cursor->events.pinch_end, &input->pinch_end, handle_pinch_end);
    vela_listen(&cursor->events.hold_begin, &input->hold_begin, handle_hold_begin);
    vela_listen(&cursor->events.hold_end, &input->hold_end, handle_hold_end);
    vela_listen(&cursor->events.frame, &input->frame, handle_frame);

    vela_listen(&server->backend->events.new_input, &input->new_input, handle_new_input);
    struct wlr_seat *seat = wlr_seat_create(display, "seat0");
    server->seat = seat;
    vela_listen(&seat->events.request_set_cursor, &input->request_set_cursor, handle_request_set_cursor);
    vela_listen(&seat->events.request_set_selection, &input->request_set_selection, handle_request_set_selection);
    vela_listen(&seat->events.request_set_primary_selection, &input->request_set_primary_selection,
        handle_request_set_primary_selection);
    vela_listen(&seat->events.request_start_drag, &input->request_start_drag, handle_request_start_drag);
    vela_listen(&seat->events.start_drag, &input->start_drag, handle_start_drag);
    struct wlr_cursor_shape_manager_v1 *shapes = wlr_cursor_shape_manager_v1_create(display, 1);
    vela_listen(&shapes->events.request_set_shape, &input->request_set_shape, handle_request_set_shape);

    wl_list_init(&input->new_virtual_pointer.link);
    wl_list_init(&input->new_virtual_keyboard.link);
    if (vela_env_int("VELA_DEBUG_INPUT", 0) != 0) {
        wlr_log(WLR_INFO, "VELA_DEBUG_INPUT: virtual pointer and keyboard enabled");
        struct wlr_virtual_pointer_manager_v1 *pointers = wlr_virtual_pointer_manager_v1_create(display);
        vela_listen(&pointers->events.new_virtual_pointer, &input->new_virtual_pointer, handle_new_virtual_pointer);
        struct wlr_virtual_keyboard_manager_v1 *keyboards = wlr_virtual_keyboard_manager_v1_create(display);
        vela_listen(&keyboards->events.new_virtual_keyboard, &input->new_virtual_keyboard, handle_new_virtual_keyboard);
    }
    return input;
}

void vela_input_destroy(struct vela_input *input)
{
    if (!input) {
        return;
    }
    // wlroots checks that no listener stays attached to the objects it
    // destroys: keyboards and mice are detached here, before the backend.
    struct vela_keyboard *keyboard, *next_keyboard;
    wl_list_for_each_safe (keyboard, next_keyboard, &input->keyboards, link) {
        vela_keyboard_destroy(keyboard);
    }
    struct vela_pointer *pointer, *next_pointer;
    wl_list_for_each_safe (pointer, next_pointer, &input->pointers, link) {
        pointer_destroy(pointer);
    }
    drag_icon_destroy(input->drag_icon);
    if (input->paste_timer) {
        wl_event_source_remove(input->paste_timer);
    }
    struct wl_listener *listeners[] = {
        &input->new_input,
        &input->new_virtual_pointer,
        &input->new_virtual_keyboard,
        &input->new_constraint,
        &input->new_inhibitor,
        &input->request_set_shape,
        &input->request_set_cursor,
        &input->request_set_selection,
        &input->request_set_primary_selection,
        &input->request_start_drag,
        &input->start_drag,
        &input->motion,
        &input->motion_absolute,
        &input->button,
        &input->axis,
        &input->frame,
        &input->swipe_begin,
        &input->swipe_update,
        &input->swipe_end,
        &input->pinch_begin,
        &input->pinch_update,
        &input->pinch_end,
        &input->hold_begin,
        &input->hold_end,
    };
    for (size_t i = 0; i < sizeof(listeners) / sizeof(listeners[0]); ++i) {
        wl_list_remove(&listeners[i]->link);
    }
    free(input);
}
