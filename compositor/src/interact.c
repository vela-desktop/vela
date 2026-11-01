// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "interact.h"

#include "a11y.h"
#include "bindings.h"
#include "decoration.h"
#include "focus.h"
#include "input.h"
#include "layer.h"
#include "lock.h"
#include "scene/scene.h"
#include "scene/surface.h"
#include "server.h"
#include "snap.h"
#include "util.h"
#include "view.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>
#include <xkbcommon/xkbcommon.h>

static void finish_keyboard(struct vela_server *server, bool confirm);

#define BORDER_BAND 8.0 // outside the window, like Windows 11
#define BORDER_INNER 4.0 // at the top also inside the title bar
#define BORDER_CORNER 16.0 // corners also take a stretch of the sides
#define DOUBLE_CLICK_MS 400

struct vela_interaction *vela_interaction_create(void)
{
    return calloc(1, sizeof(struct vela_interaction));
}

void vela_interaction_destroy(struct vela_interaction *interaction)
{
    free(interaction);
}

static struct vela_view *view_of(struct vela_owner *owner)
{
    return owner && owner->kind == VELA_OWNER_VIEW ? (struct vela_view *)owner : NULL;
}

static const char *resize_cursor(uint32_t edges)
{
    bool top = edges & WLR_EDGE_TOP;
    bool bottom = edges & WLR_EDGE_BOTTOM;
    bool left = edges & WLR_EDGE_LEFT;
    bool right = edges & WLR_EDGE_RIGHT;
    if (top) {
        return left ? "nw-resize" : right ? "ne-resize" : "n-resize";
    }
    if (bottom) {
        return left ? "sw-resize" : right ? "se-resize" : "s-resize";
    }
    return left ? "w-resize" : "e-resize";
}

// The windows of `tree` from the topmost: true at the first one covering the
// point or with its border there (*found: that window, NULL if it covers it).
static bool border_in(struct vela_tree *tree, double lx, double ly, struct vela_view **found, uint32_t *edges)
{
    struct vela_node *child;
    wl_list_for_each_reverse (child, &tree->children, link) {
        struct vela_view *view = view_of(child->data);
        if (!view || !child->enabled || !view->mapped || view->minimized) {
            continue;
        }
        struct wlr_box f = vela_view_frame_box(view);
        bool resizable = view->decoration && !view->maximized && !view->fullscreen && vela_view_resizable(view);
        bool inside = lx >= f.x && lx < f.x + f.width && ly >= f.y && ly < f.y + f.height;
        if (inside && !(resizable && ly < f.y + BORDER_INNER)) {
            *found = NULL; // the window covers the point
            return true;
        }
        if (!resizable || lx < f.x - BORDER_BAND || lx >= f.x + f.width + BORDER_BAND || ly < f.y - BORDER_BAND
            || ly >= f.y + f.height + BORDER_BAND) {
            continue;
        }
        bool left = lx < f.x;
        bool right = lx >= f.x + f.width;
        bool top = ly < f.y + (inside ? BORDER_INNER : 0.0);
        bool bottom = ly >= f.y + f.height;
        if (top || bottom) {
            left = left || lx < f.x + BORDER_CORNER;
            right = right || (!left && lx >= f.x + f.width - BORDER_CORNER);
        }
        if (left || right) {
            top = top || ly < f.y + BORDER_CORNER;
            bottom = bottom || (!top && ly >= f.y + f.height - BORDER_CORNER);
        }
        *edges = (top ? WLR_EDGE_TOP : 0) | (bottom ? WLR_EDGE_BOTTOM : 0) | (left ? WLR_EDGE_LEFT : 0)
            | (right ? WLR_EDGE_RIGHT : 0);
        *found = *edges ? view : NULL;
        return true;
    }
    return false;
}

// The invisible borders to resize windows with Vela's bar, like Windows 11:
// the window and the edges (WLR_EDGE_*) under the point, or NULL.
static struct vela_view *resize_border_at(struct vela_server *server, double lx, double ly, uint32_t *edges)
{
    *edges = 0;
    // Above the windows (taskbar, menus, panels): no borders.
    struct vela_hit hit = vela_scene_at(server->scene, lx, ly);
    struct vela_owner *owner = hit.owner;
    if (owner && owner->kind == VELA_OWNER_LAYER) {
        uint32_t layer = ((struct vela_layer_surface *)owner)->layer;
        if (layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP || layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
            return NULL;
        }
    }
    // From the topmost window: the first covering the point wins. A game in
    // front has its own layer, above the windows: the borders of the ones
    // it covers are not there.
    struct vela_view *view = NULL;
    if (border_in(server->layers.fullscreen, lx, ly, &view, edges)
        || border_in(server->layers.windows, lx, ly, &view, edges)) {
        return view;
    }
    return NULL;
}

static void unhover_decoration(struct vela_interaction *interaction)
{
    struct vela_view *hovered = interaction->hovered_decoration;
    if (hovered && hovered->decoration) {
        vela_decoration_set_hover(hovered->decoration, VELA_DECORATION_NONE);
    }
}

// The visible lock scene is the only eligible pointer target during a lock.
// Focus changes (map/hotplug) need no synthetic motion event.
/** Focus only a visible lock surface, optionally delivering real mouse motion. */
static void lock_pointer_focus(struct vela_server *server, uint32_t time_msec, bool notify_motion)
{
    struct wlr_seat *seat = server->seat;
    struct wlr_cursor *cursor = server->cursor;
    struct vela_hit hit = vela_scene_at(server->scene, cursor->x, cursor->y);
    if (hit.surface) {
        wlr_seat_pointer_notify_enter(seat, hit.surface, hit.sx, hit.sy);
        if (notify_motion) {
            wlr_seat_pointer_notify_motion(seat, time_msec, hit.sx, hit.sy);
        }
    } else {
        wlr_seat_pointer_clear_focus(seat);
        wlr_cursor_set_xcursor(cursor, server->cursor_manager, "default");
    }
}

/** Recompute lock-surface pointer focus without synthesizing motion. */
void vela_interact_retarget_lock_pointer(struct vela_server *server)
{
    if (server->locked) {
        lock_pointer_focus(server, 0, false);
    }
}

/** Route real pointer movement to the currently interactive scene. */
void vela_interact_motion(struct vela_server *server, uint32_t time_msec)
{
    struct vela_interaction *interaction = server->interaction;
    struct wlr_cursor *cursor = server->cursor;
    struct wlr_seat *seat = server->seat;
    struct vela_input *input = server->input;

    // The desktop is still represented in memory while ext-session-lock is
    // active, but it is not eligible for interaction. In particular,
    // resize_border_at() walks the windows layer directly, and the disabled
    // parent layer alone does not hide its children from that lookup. Only
    // hit-test the visible lock scene while locked; otherwise an invisible
    // window's resize band steals pointer focus and changes the cursor.
    if (server->locked) {
        struct vela_a11y *a11y = server->a11y;
        if (a11y->magnifier || a11y->zoom_animating || a11y->zoom > 1.0) {
            vela_a11y_update_magnifier(a11y);
        }
        lock_pointer_focus(server, time_msec, true);
        return;
    }

    vela_input_update_drag_icon(input);
    struct vela_a11y *a11y = server->a11y;
    if (a11y->magnifier || a11y->zoom_animating || a11y->zoom > 1.0) {
        vela_a11y_update_magnifier(a11y); // the zoomed area follows the cursor
    }
    struct vela_view *dragged = interaction->pending_title_drag.view;
    if (dragged
        && hypot(cursor->x - interaction->pending_title_drag.x, cursor->y - interaction->pending_title_drag.y) > 4.0) {
        interaction->pending_title_drag.view = NULL;
        // A real drag is not the first click of a double-click sequence.
        // Clearing it also allows repeated small title-bar drags without an
        // artificial 400 ms delay (important for precision pixel tests).
        if (interaction->last_title_click.view == dragged) {
            memset(&interaction->last_title_click, 0, sizeof(interaction->last_title_click));
        }
        vela_interact_begin(server, dragged, VELA_CURSOR_MOVE, 0, true);
    }
    struct vela_view *grabbed = server->grabbed;
    if (server->cursor_mode == VELA_CURSOR_MOVE && grabbed) {
        // Exact position, fractional too: when drawn the window snaps to the
        // nearest physical pixel (§3.4). With integer logical positions, at
        // 125% the window would advance in jumps of 1 and 2 pixels.
        vela_node_set_position(&grabbed->tree->node, cursor->x - interaction->grab_x, cursor->y - interaction->grab_y);
        // A focused window crossing onto a fullscreen output must stay visible
        // without forcing the game out of its protocol fullscreen state.
        vela_fullscreen_restack(server);
        vela_snap_update_zone(server);
        return;
    }
    if (server->cursor_mode == VELA_CURSOR_RESIZE && grabbed) {
        double border_x = cursor->x - interaction->grab_x;
        double border_y = cursor->y - interaction->grab_y;
        const struct wlr_box *box = &interaction->grab_box;
        int left = box->x;
        int right = box->x + box->width;
        int top = box->y;
        int bottom = box->y + box->height;
        uint32_t edges = interaction->resize_edges;
        if (edges & WLR_EDGE_TOP) {
            top = vela_min((int)border_y, bottom - 1);
        } else if (edges & WLR_EDGE_BOTTOM) {
            bottom = vela_max((int)border_y, top + 1);
        }
        if (edges & WLR_EDGE_LEFT) {
            left = vela_min((int)border_x, right - 1);
        } else if (edges & WLR_EDGE_RIGHT) {
            right = vela_max((int)border_x, left + 1);
        }
        struct wlr_box geometry = vela_view_geometry(grabbed);
        vela_node_set_position(&grabbed->tree->node, left - geometry.x, top - geometry.y);
        vela_view_configure_size(grabbed, right - left, bottom - top);
        return;
    }

    // A button pressed on a surface: motion stays with it until released (a
    // rubber-band selection leaving the output, a scroll bar dragged outside
    // the window).
    if (input->implicit_grab.surface && !seat->drag) {
        if (seat->pointer_state.button_count > 0 && seat->pointer_state.focused_surface == input->implicit_grab.surface) {
            double density = vela_surface_density(input->implicit_grab.surface);
            wlr_seat_pointer_notify_motion(seat, time_msec, (cursor->x - input->implicit_grab.origin_x) * density,
                (cursor->y - input->implicit_grab.origin_y) * density);
            return;
        }
        memset(&input->implicit_grab, 0, sizeof(input->implicit_grab));
    }

    // On the border of a window with Vela's bar: the resize arrows.
    if (server->cursor_mode == VELA_CURSOR_PASSTHROUGH && seat->pointer_state.button_count == 0 && !seat->drag) {
        uint32_t edges = 0;
        if (resize_border_at(server, cursor->x, cursor->y, &edges)) {
            unhover_decoration(interaction);
            interaction->hovered_decoration = NULL;
            wlr_seat_pointer_clear_focus(seat);
            vela_input_constrain(input, NULL);
            wlr_cursor_set_xcursor(cursor, server->cursor_manager, resize_cursor(edges));
            return;
        }
    }

    struct vela_hit hit = vela_scene_at(server->scene, cursor->x, cursor->y);
    // Over Vela's title bar: the buttons light up.
    struct vela_view *decorated = hit.surface ? NULL : view_of(hit.owner);
    if (decorated && !decorated->decoration) {
        decorated = NULL;
    }
    if (interaction->hovered_decoration != decorated) {
        unhover_decoration(interaction);
    }
    interaction->hovered_decoration = decorated;
    if (decorated) {
        enum vela_decoration_part part = vela_decoration_part_at(decorated->decoration, cursor->x, cursor->y);
        vela_decoration_set_hover(decorated->decoration, part);
        vela_snap_hover_maximize(server, part == VELA_DECORATION_MAXIMIZE ? decorated : NULL);
    } else {
        vela_snap_hover_maximize(server, NULL);
    }
    if (!hit.surface) {
        wlr_cursor_set_xcursor(cursor, server->cursor_manager, "default");
        wlr_seat_pointer_clear_focus(seat);
        vela_input_constrain(input, NULL);
        return;
    }
    wlr_seat_pointer_notify_enter(seat, hit.surface, hit.sx, hit.sy);
    wlr_seat_pointer_notify_motion(seat, time_msec, hit.sx, hit.sy);
    vela_input_constrain(input, hit.surface);
}

static void decoration_press(struct vela_server *server, struct vela_view *view, uint32_t time_msec)
{
    struct vela_interaction *interaction = server->interaction;
    struct wlr_cursor *cursor = server->cursor;
    vela_focus_view(server, view);
    switch (vela_decoration_part_at(view->decoration, cursor->x, cursor->y)) {
    case VELA_DECORATION_CLOSE:
        vela_view_close(view);
        return;
    case VELA_DECORATION_MAXIMIZE:
        vela_view_set_maximized(view, !view->maximized, true);
        return;
    case VELA_DECORATION_MINIMIZE:
        vela_view_set_minimized(view, true);
        return;
    case VELA_DECORATION_ICON: {
        // Like Windows: a click on the icon opens the window menu, a double
        // click closes the window.
        bool double_click = interaction->last_icon_click.view == view
            && time_msec - interaction->last_icon_click.time_msec < DOUBLE_CLICK_MS;
        interaction->last_icon_click.view = view;
        interaction->last_icon_click.time_msec = time_msec;
        if (double_click) {
            memset(&interaction->last_icon_click, 0, sizeof(interaction->last_icon_click));
            vela_view_close(view);
            return;
        }
        struct wlr_box frame = vela_view_frame_box(view);
        vela_window_menu_show(server, view, frame.x + VELA_DECORATION_ICON_X - 4, frame.y + VELA_DECORATION_HEIGHT,
            false);
        return;
    }
    case VELA_DECORATION_TITLE: {
        // Double click: maximize or restore, like Windows.
        bool double_click = interaction->last_title_click.view == view
            && time_msec - interaction->last_title_click.time_msec < DOUBLE_CLICK_MS;
        interaction->last_title_click.view = view;
        interaction->last_title_click.time_msec = time_msec;
        if (double_click) {
            memset(&interaction->last_title_click, 0, sizeof(interaction->last_title_click));
            vela_view_set_maximized(view, !view->maximized, true);
            return;
        }
        // The drag starts only if the mouse really moves: a click (or the
        // first of a double click) must not restore a maximized window.
        interaction->pending_title_drag.view = view;
        interaction->pending_title_drag.x = cursor->x;
        interaction->pending_title_drag.y = cursor->y;
        interaction->modifier_grab = true; // the release doesn't go to the app
        return;
    }
    case VELA_DECORATION_NONE:
        return;
    }
}

void vela_interact_button(struct vela_server *server, struct wlr_pointer_button_event *event)
{
    struct vela_interaction *interaction = server->interaction;
    struct wlr_cursor *cursor = server->cursor;
    struct wlr_seat *seat = server->seat;
    struct vela_input *input = server->input;
    bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
    input->super_tap = false;
    vela_lock_note_activity(server->lock);
    // Locked: the click gives the keyboard to the lock screen under the mouse
    // (with several outputs) and reaches only it.
    if (server->locked) {
        wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
        if (pressed) {
            struct vela_hit hit = vela_scene_at(server->scene, cursor->x, cursor->y);
            if (hit.surface) {
                vela_input_keyboard_enter(input, hit.surface);
            }
        }
        return;
    }

    // Keyboard "Move" or "Size" in progress: a click confirms.
    if (interaction->keyboard.view && pressed) {
        finish_keyboard(server, true);
        interaction->modifier_grab = true; // not even the release reaches the app
        return;
    }

    // On the border of a window with Vela's bar: resize.
    if (pressed && event->button == BTN_LEFT && server->cursor_mode == VELA_CURSOR_PASSTHROUGH
        && seat->pointer_state.button_count == 0) {
        uint32_t edges = 0;
        struct vela_view *view = resize_border_at(server, cursor->x, cursor->y, &edges);
        if (view) {
            vela_focus_view(server, view);
            vela_interact_begin(server, view, VELA_CURSOR_RESIZE, edges, true);
            if (server->cursor_mode != VELA_CURSOR_PASSTHROUGH) {
                interaction->modifier_grab = true; // not even the release reaches the app
                return;
            }
        }
    }

    // Super + drag moves the window, Super + right button resizes it (from the
    // nearest corner), like KDE. X11 windows without a title bar of their own
    // need it too. The click doesn't reach the app.
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    bool super = keyboard && (wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_LOGO);
    if (pressed && super && server->cursor_mode == VELA_CURSOR_PASSTHROUGH
        && (event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
        struct vela_view *view = view_of(vela_scene_at(server->scene, cursor->x, cursor->y).owner);
        if (view) {
            vela_focus_view(server, view);
            uint32_t edges = 0;
            if (event->button == BTN_RIGHT) {
                struct wlr_box frame = vela_view_frame_box(view);
                edges |= cursor->x < frame.x + frame.width / 2.0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
                edges |= cursor->y < frame.y + frame.height / 2.0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
            }
            vela_interact_begin(server, view, event->button == BTN_LEFT ? VELA_CURSOR_MOVE : VELA_CURSOR_RESIZE,
                edges, true);
            if (server->cursor_mode != VELA_CURSOR_PASSTHROUGH) {
                interaction->modifier_grab = true;
                return;
            }
        }
    }
    if (interaction->modifier_grab && !pressed) {
        interaction->modifier_grab = false; // the press hadn't reached the app
        memset(&interaction->pending_title_drag, 0, sizeof(interaction->pending_title_drag));
    } else {
        // The first button pressed on a surface "grabs" it (see
        // vela_input.implicit_grab).
        if (pressed && seat->pointer_state.button_count == 0 && seat->pointer_state.focused_surface && !seat->drag) {
            input->implicit_grab.surface = seat->pointer_state.focused_surface;
            double density = vela_surface_density(input->implicit_grab.surface);
            input->implicit_grab.origin_x = cursor->x - seat->pointer_state.sx / density;
            input->implicit_grab.origin_y = cursor->y - seat->pointer_state.sy / density;
        }
        wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
    }

    if (!pressed) {
        // All buttons released: the pointer goes back to what's under it.
        if (input->implicit_grab.surface && seat->pointer_state.button_count == 0) {
            memset(&input->implicit_grab, 0, sizeof(input->implicit_grab));
            if (server->cursor_mode == VELA_CURSOR_PASSTHROUGH) {
                vela_interact_motion(server, event->time_msec);
            }
        }
        if (server->cursor_mode != VELA_CURSOR_PASSTHROUGH) {
            if (server->cursor_mode == VELA_CURSOR_MOVE) {
                vela_snap_end_zone(server, true); // released on an edge: it snaps
            }
            server->cursor_mode = VELA_CURSOR_PASSTHROUGH;
            server->grabbed = NULL;
            vela_interact_motion(server, event->time_msec);
        }
        return;
    }

    struct vela_hit hit = vela_scene_at(server->scene, cursor->x, cursor->y);
    struct vela_owner *owner = hit.owner;
    if (!owner) {
        return;
    }
    // Vela's title bar: buttons, drag, double click; with the right button the
    // window menu.
    struct vela_view *view = view_of(owner);
    if (view && !hit.surface && view->decoration) {
        if (event->button == BTN_LEFT) {
            decoration_press(server, view, event->time_msec);
            return;
        }
        if (event->button == BTN_RIGHT
            && vela_decoration_part_at(view->decoration, cursor->x, cursor->y) == VELA_DECORATION_TITLE) {
            vela_focus_view(server, view);
            vela_window_menu_show(server, view, cursor->x, cursor->y, false);
            return;
        }
    }
    if (view) {
        vela_focus_view(server, view);
    } else {
        struct vela_layer_surface *layer = (struct vela_layer_surface *)owner;
        if (vela_layer_surface_wants_keyboard(layer)) {
            vela_focus_layer(server, layer);
        }
    }
}

void vela_interact_begin(struct vela_server *server, struct vela_view *view, int mode, uint32_t edges,
    bool from_modifier)
{
    struct vela_interaction *interaction = server->interaction;
    struct wlr_cursor *cursor = server->cursor;
    // An app's request is accepted only from the window the pointer is on.
    struct wlr_surface *focused = server->seat->pointer_state.focused_surface;
    if (!from_modifier && (!focused || wlr_surface_get_root_surface(focused) != vela_view_surface(view))) {
        return;
    }
    if (view->fullscreen) {
        return;
    }
    vela_view_finish_open_animation(view);

    // Dragging a maximized or snapped window restores it under the cursor,
    // keeping the grabbed point at the same proportion and the title bar under
    // the cursor (like Windows).
    if ((view->maximized || !vela_snap_is_none(view->snap)) && mode == VELA_CURSOR_MOVE) {
        struct wlr_box frame = vela_view_frame_box(view);
        double fraction = frame.width > 0 ? (cursor->x - frame.x) / frame.width : 0.5;
        int restored_width = vela_view_restore_box(view).width;
        if (view->maximized) {
            vela_view_set_maximized(view, false, false);
        } else {
            vela_view_set_snap(view, vela_snap_none, NULL, false);
        }
        struct wlr_box geometry = vela_view_geometry(view);
        vela_node_set_position(&view->tree->node, (int)(cursor->x - fraction * restored_width) - geometry.x,
            frame.y - geometry.y);
    }
    // Resizing a snapped window unsnaps it, leaving it where it is.
    if (!vela_snap_is_none(view->snap) && mode == VELA_CURSOR_RESIZE) {
        view->snap = vela_snap_none;
        vela_view_send_tiled(view, WLR_EDGE_NONE);
    }

    server->grabbed = view;
    server->cursor_mode = mode;
    if (mode == VELA_CURSOR_MOVE) {
        interaction->grab_x = cursor->x - view->tree->node.x;
        interaction->grab_y = cursor->y - view->tree->node.y;
        return;
    }
    struct wlr_box frame = vela_view_frame_box(view);
    double border_x = frame.x + ((edges & WLR_EDGE_RIGHT) ? frame.width : 0);
    double border_y = frame.y + ((edges & WLR_EDGE_BOTTOM) ? frame.height : 0);
    interaction->grab_x = cursor->x - border_x;
    interaction->grab_y = cursor->y - border_y;
    interaction->grab_box = frame;
    interaction->resize_edges = edges;
}

// ------------------------------------------------------ from the keyboard --

void vela_interact_begin_keyboard(struct vela_server *server, struct vela_view *view, int mode)
{
    struct vela_interaction *interaction = server->interaction;
    struct wlr_cursor *cursor = server->cursor;
    if (view->minimized || view->maximized || view->fullscreen || server->cursor_mode != VELA_CURSOR_PASSTHROUGH) {
        return;
    }
    // Not under the polkit dialog: the keys are the dialog's (the window
    // menu can still ask for it from the shell).
    if (vela_focus_modal_layer(server)) {
        return;
    }
    if (!vela_snap_is_none(view->snap)) {
        vela_view_set_snap(view, vela_snap_none, NULL, false);
    }
    vela_focus_view(server, view);
    vela_view_finish_open_animation(view);
    struct wlr_box frame = vela_view_frame_box(view);
    interaction->keyboard.view = view;
    interaction->keyboard.mode = mode;
    interaction->keyboard.edge_chosen = false;
    interaction->keyboard.tree_x = view->tree->node.x;
    interaction->keyboard.tree_y = view->tree->node.y;
    interaction->keyboard.geometry = vela_view_geometry(view);
    // Like Windows: the pointer goes to the title bar (move) or the center of
    // the window (size), and follows it from there.
    if (mode == VELA_CURSOR_MOVE) {
        wlr_cursor_warp(cursor, NULL, frame.x + frame.width / 2.0, frame.y + vela_min(16, frame.height / 2));
        vela_interact_begin(server, view, VELA_CURSOR_MOVE, 0, true);
    } else {
        wlr_cursor_warp(cursor, NULL, frame.x + frame.width / 2.0, frame.y + frame.height / 2.0);
    }
    wlr_seat_pointer_clear_focus(server->seat);
    wlr_cursor_set_xcursor(cursor, server->cursor_manager, mode == VELA_CURSOR_MOVE ? "move" : "all-scroll");
}

static void keyboard_key(struct vela_server *server, xkb_keysym_t sym, uint32_t modifiers)
{
    struct vela_interaction *interaction = server->interaction;
    struct vela_view *view = interaction->keyboard.view;
    double step = (modifiers & WLR_MODIFIER_CTRL) ? 1.0 : 10.0;
    double dx = 0.0;
    double dy = 0.0;
    uint32_t edge = 0;
    switch (sym) {
    case XKB_KEY_Left:
        dx = -step;
        edge = WLR_EDGE_LEFT;
        break;
    case XKB_KEY_Right:
        dx = step;
        edge = WLR_EDGE_RIGHT;
        break;
    case XKB_KEY_Up:
        dy = -step;
        edge = WLR_EDGE_TOP;
        break;
    case XKB_KEY_Down:
        dy = step;
        edge = WLR_EDGE_BOTTOM;
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        finish_keyboard(server, true);
        return;
    case XKB_KEY_Escape:
        finish_keyboard(server, false);
        return;
    default:
        return;
    }
    if (interaction->keyboard.mode == VELA_CURSOR_RESIZE && !interaction->keyboard.edge_chosen) {
        // The first arrow key chooses the edge to move.
        struct wlr_box frame = vela_view_frame_box(view);
        double x = edge == WLR_EDGE_LEFT ? frame.x
            : edge == WLR_EDGE_RIGHT     ? frame.x + frame.width
                                         : frame.x + frame.width / 2.0;
        double y = edge == WLR_EDGE_TOP ? frame.y
            : edge == WLR_EDGE_BOTTOM   ? frame.y + frame.height
                                        : frame.y + frame.height / 2.0;
        wlr_cursor_warp(server->cursor, NULL, x, y);
        vela_interact_begin(server, view, VELA_CURSOR_RESIZE, edge, true);
        interaction->keyboard.edge_chosen = true;
        return;
    }
    wlr_cursor_move(server->cursor, NULL, dx, dy);
    vela_interact_motion(server, 0);
}

bool vela_interact_keyboard(struct vela_server *server, const uint32_t *syms, int count, uint32_t modifiers,
    bool pressed)
{
    if (!server->interaction->keyboard.view) {
        return false;
    }
    for (int i = 0; pressed && i < count && server->interaction->keyboard.view; ++i) {
        keyboard_key(server, syms[i], modifiers);
    }
    return true;
}

static void finish_keyboard(struct vela_server *server, bool confirm)
{
    struct vela_interaction *interaction = server->interaction;
    struct vela_view *view = interaction->keyboard.view;
    if (!view) {
        return;
    }
    if (!confirm) {
        // Esc: back to how it was.
        vela_node_set_position(&view->tree->node, interaction->keyboard.tree_x, interaction->keyboard.tree_y);
        if (interaction->keyboard.mode == VELA_CURSOR_RESIZE && interaction->keyboard.edge_chosen) {
            vela_view_configure_size(view, interaction->keyboard.geometry.width,
                interaction->keyboard.geometry.height);
        }
    }
    vela_snap_end_zone(server, false);
    memset(&interaction->keyboard, 0, sizeof(interaction->keyboard));
    server->cursor_mode = VELA_CURSOR_PASSTHROUGH;
    server->grabbed = NULL;
    wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
    vela_interact_motion(server, 0);
}

void vela_interact_cancel_keyboard(struct vela_server *server)
{
    finish_keyboard(server, false);
}

/** Cancel every compositor interaction that must not outlive a screen lock. */
void vela_interact_cancel_for_lock(struct vela_server *server)
{
    struct vela_interaction *interaction = server->interaction;
    // Move/Size from the keyboard otherwise consumes the locker's keystrokes.
    // Cancel it as Escape would, restoring the original window geometry.
    finish_keyboard(server, false);
    unhover_decoration(interaction);
    interaction->hovered_decoration = NULL;
    memset(&interaction->pending_title_drag, 0, sizeof(interaction->pending_title_drag));
    memset(&interaction->last_title_click, 0, sizeof(interaction->last_title_click));
    memset(&interaction->last_icon_click, 0, sizeof(interaction->last_icon_click));
    interaction->modifier_grab = false;
    server->cursor_mode = VELA_CURSOR_PASSTHROUGH;
    server->grabbed = NULL;
    server->input->super_tap = false;
    memset(&server->input->implicit_grab, 0, sizeof(server->input->implicit_grab));
}

void vela_interact_forget(struct vela_server *server, struct vela_view *view)
{
    struct vela_interaction *interaction = server->interaction;
    if (interaction->hovered_decoration == view) {
        interaction->hovered_decoration = NULL;
    }
    if (interaction->last_title_click.view == view) {
        memset(&interaction->last_title_click, 0, sizeof(interaction->last_title_click));
    }
    if (interaction->last_icon_click.view == view) {
        memset(&interaction->last_icon_click, 0, sizeof(interaction->last_icon_click));
    }
    if (interaction->pending_title_drag.view == view) {
        memset(&interaction->pending_title_drag, 0, sizeof(interaction->pending_title_drag));
    }
    if (interaction->keyboard.view == view) {
        memset(&interaction->keyboard, 0, sizeof(interaction->keyboard));
    }
}
