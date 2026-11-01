// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_INTERACT_H
#define VELA_INTERACT_H

// The pointer on windows, like Windows 11: pointer focus and the implicit
// grab, moving and resizing (from Vela's bar, from the invisible borders
// around windows, with Super + drag, or when the app asks), the title bar
// buttons and double click; and keyboard "Move"/"Size" (arrows, by one unit
// with Ctrl; Enter confirms, Esc cancels; the mouse moves too).
//
// The grabbed window and the mode live in vela_server (grabbed, cursor_mode);
// the rest here.

#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>

struct vela_server;
struct vela_view;
struct wlr_pointer_button_event;

struct vela_interaction {
    // The grabbed point, relative to the window origin (move) or to the moving
    // edge (resize).
    double grab_x;
    double grab_y;
    struct wlr_box grab_box; // the frame when resizing started
    uint32_t resize_edges;
    bool modifier_grab; // the press didn't reach the app: neither does the release
    // Vela's title bar under the mouse, and the last clicks on title and icon
    // (for double clicks).
    struct vela_view *hovered_decoration;
    struct {
        struct vela_view *view;
        uint32_t time_msec;
    } last_title_click, last_icon_click;
    // The title pressed: the drag starts only if the mouse really moves.
    struct {
        struct vela_view *view;
        double x;
        double y;
    } pending_title_drag;
    // Keyboard "Move"/"Size".
    struct {
        struct vela_view *view;
        int mode; // enum vela_cursor_mode
        bool edge_chosen; // resizing: the first arrow key chooses the edge
        double tree_x; // as it was, for Esc
        double tree_y;
        struct wlr_box geometry;
    } keyboard;
};

struct vela_interaction *vela_interaction_create(void);
void vela_interaction_destroy(struct vela_interaction *interaction);

// The cursor moved (or what's under it changed).
void vela_interact_motion(struct vela_server *server, uint32_t time_msec);
void vela_interact_button(struct vela_server *server, struct wlr_pointer_button_event *event);
// Starts moving or resizing (`edges`: WLR_EDGE_*). Requests from apps count
// only from the window under the pointer (from_modifier: false).
void vela_interact_begin(struct vela_server *server, struct vela_view *view, int mode, uint32_t edges,
    bool from_modifier);

// Keyboard "Move"/"Size" (mode: enum vela_cursor_mode).
void vela_interact_begin_keyboard(struct vela_server *server, struct vela_view *view, int mode);
// While it runs, keys are its own (true) and presses move the window.
bool vela_interact_keyboard(struct vela_server *server, const uint32_t *syms, int count, uint32_t modifiers,
    bool pressed);
// Ends a keyboard Move/Size as Esc would (the window goes back): the polkit
// dialog takes the keyboard, and its keys must reach it.
void vela_interact_cancel_keyboard(struct vela_server *server);
// Discard any desktop interaction before handing input to a session locker.
void vela_interact_cancel_for_lock(struct vela_server *server);
// Refresh the lock-surface pointer focus without synthesizing mouse movement.
void vela_interact_retarget_lock_pointer(struct vela_server *server);

// The window goes away: forget it.
void vela_interact_forget(struct vela_server *server, struct vela_view *view);

#endif
