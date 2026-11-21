// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_INPUT_H
#define VELA_INPUT_H

// The seat and input devices, like Windows 11:
//
// - mouse: speed, "Enhance pointer precision" (acceleration), primary
//   button, lines per wheel notch;
// - touchpad: on or off (also only while a mouse is plugged in), speed, tap
//   to click (two fingers: right button), scroll direction, no accidental
//   touches while typing;
// - three- and four-finger gestures: up for Task View, down for the desktop,
//   sideways to switch app (Alt+Tab, following the fingers) or virtual
//   desktop. Other gestures (pinch, swipes that aren't ours) go to the apps
//   (pointer-gestures);
// - games and apps that want the mouse to themselves: relative motion
//   (relative-pointer) and a pointer locked or confined to the window
//   (pointer-constraints); a focused app can keep the shortcuts to itself
//   (keyboard-shortcuts-inhibit: virtual machines, remote desktop);
// - clipboard, drag between apps (with the icon following the cursor),
//   cursor shape asked by name.
//
// The choices are in vela.conf; VELA_NATURAL_SCROLL=0 wins over the
// direction. Keyboards are in keyboard.h. Creates the server's seat and
// cursor.

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

#include "super_numbers.h"

struct vela_server;
struct vela_tree;
struct vela_surface_node;
struct wlr_pointer_constraint_v1;
struct wlr_pointer_constraints_v1;
struct wlr_pointer_gestures_v1;
struct wlr_relative_pointer_manager_v1;
struct wlr_keyboard_shortcuts_inhibit_manager_v1;
struct wlr_surface;

enum vela_swipe_action {
    VELA_SWIPE_NONE,
    VELA_SWIPE_APP, // sideways: switch app (Alt+Tab)
    VELA_SWIPE_DESKTOP, // sideways: switch virtual desktop
    VELA_SWIPE_OTHER, // an unknown value in vela.conf: only up and down
};

// An icon dragged between apps: follows the cursor, offset as the app asks.
struct vela_drag_icon {
    struct vela_input *input;
    struct wlr_surface *surface;
    struct vela_tree *tree;
    struct vela_surface_node *node;
    double dx;
    double dy;
    struct wl_listener commit;
    struct wl_listener destroy;
};

struct vela_input {
    struct vela_server *server;
    struct wl_list pointers; // vela_pointer.link
    struct wl_list keyboards; // vela_keyboard.link

    // From vela.conf.
    double wheel_factor; // lines per notch / 3
    enum vela_swipe_action three_fingers;
    enum vela_swipe_action four_fingers;

    // The multi-finger swipe in progress: ours (an action) or the app's.
    struct {
        bool ours;
        enum vela_swipe_action action;
        int fingers;
        double dx;
        double dy;
        int steps; // "switch app" steps already taken
    } swipe;

    // Super pressed and released alone: opens the Start menu.
    bool super_tap;
    enum vela_super_numbers super_numbers;
    struct wl_event_source *super_timer;
    struct vela_keyboard *super_keyboard;
    bool super_hints_visible;
    // The layout written to the log last time (keyboard.c): only changes are
    // logged.
    char keymap_logged[1024];

    // Implicit grab, as Wayland wants: while a button is held, the pointer
    // stays with the surface it was pressed on (even outside it, even on
    // another output), which so gets the release too.
    struct {
        struct wlr_surface *surface;
        double origin_x; // where its origin is in the layout
        double origin_y;
    } implicit_grab;

    struct vela_drag_icon *drag_icon;

    struct wlr_pointer_gestures_v1 *gestures;
    struct wlr_relative_pointer_manager_v1 *relative_pointers;
    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_pointer_constraint_v1 *active_constraint;
    struct wlr_keyboard_shortcuts_inhibit_manager_v1 *shortcuts_inhibit;
    struct wl_event_source *paste_timer;
    const char *cursor_shape; // last theme shape requested by an app, NULL for a surface cursor

    struct wl_listener new_input;
    struct wl_listener new_virtual_pointer;
    struct wl_listener new_virtual_keyboard;
    struct wl_listener new_constraint;
    struct wl_listener new_inhibitor;
    struct wl_listener request_set_shape;
    struct wl_listener request_set_cursor;
    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;
    struct wl_listener request_start_drag;
    struct wl_listener start_drag;
    struct wl_listener motion;
    struct wl_listener motion_absolute;
    struct wl_listener button;
    struct wl_listener axis;
    struct wl_listener frame;
    struct wl_listener swipe_begin;
    struct wl_listener swipe_update;
    struct wl_listener swipe_end;
    struct wl_listener pinch_begin;
    struct wl_listener pinch_update;
    struct wl_listener pinch_end;
    struct wl_listener hold_begin;
    struct wl_listener hold_end;
};

// Creates the seat, cursor and input protocols. VELA_DEBUG_INPUT=1 enables
// virtual mice and keyboards (for tests: they let any program fake input).
struct vela_input *vela_input_create(struct vela_server *server);
// Before destroying cursor, backend and display.
void vela_input_destroy(struct vela_input *input);

// Rereads vela.conf ("reload-config"): keyboard layouts and repeat, mice and
// touchpads.
void vela_input_reload(struct vela_input *input);
void vela_input_reload_cursor(struct vela_input *input, const char *theme);

void vela_input_hide_super_hints(struct vela_input *input);
bool vela_input_has_touchpad(const struct vela_input *input);
// The focused app keeps the shortcuts to itself.
bool vela_input_shortcuts_inhibited(const struct vela_input *input);

// The pointer is on `surface` (NULL: none): its constraint, if any, becomes
// active.
void vela_input_constrain(struct vela_input *input, struct wlr_surface *surface);
// The keyboard to this surface (also to an X11 menu that asks), with the keys
// and modifiers held now.
void vela_input_keyboard_enter(struct vela_input *input, struct wlr_surface *surface);

// The drag icon where the cursor is.
void vela_input_update_drag_icon(struct vela_input *input);

// Win+V: pastes into the focused app what the shell has just put on the
// clipboard (Ctrl+V, Ctrl+Shift+V in terminals), a moment after the panel
// closed.
void vela_input_paste(struct vela_input *input);

#endif
