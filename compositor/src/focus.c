// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "focus.h"

#include "input.h"
#include "interact.h"
#include "layer.h"
#include "scene/scene.h"
#include "server.h"
#include "snap.h"
#include "switcher.h"
#include "view.h"
#include "workspace.h"

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_seat.h>

void vela_focus_view(struct vela_server *server, struct vela_view *view)
{
    if (!view || !view->mapped || server->locked) {
        return;
    }
    // A window on another desktop: go to that desktop, like Windows.
    if (!vela_view_on_current_workspace(view)) {
        vela_workspaces_switch(server, view->workspace, false);
    }
    if (view->minimized) {
        vela_view_set_minimized(view, false); // turns it back on and returns here
        return;
    }

    // Bring it forward and to the head of the list in any case. System dialogs
    // stay above (but the keyboard goes where the user clicked).
    // Like Windows: a fullscreen window stays above the taskbar and the other
    // windows of its output only while it is the one in use (Alt+Tab out of a
    // game shows the chosen window; back to the game, it covers all again).
    struct vela_layers *layers = &server->layers;
    struct vela_view *other;
    wl_list_for_each (other, &server->views, link) {
        if (other != view && other->tree->node.parent == layers->fullscreen
            && vela_view_output(other) == vela_view_output(view)) {
            vela_node_reparent(&other->tree->node, layers->windows);
        }
    }
    if (view->fullscreen) {
        vela_node_reparent(&view->tree->node, layers->fullscreen);
    }
    vela_node_raise_to_top(&view->tree->node);
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);
    if (!vela_view_is_system_prompt(view)) {
        struct vela_view *prompt = vela_views_system_prompt(server);
        if (prompt) {
            vela_node_raise_to_top(&prompt->tree->node);
        }
    }

    // A panel that asked for exclusive keyboard (such as the polkit dialog)
    // doesn't give it to a window.
    struct vela_layer_surface *layer = server->focused_layer;
    if ((layer && layer->wlr->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
        || vela_focus_modal_layer(server)) {
        return;
    }
    server->focused_layer = NULL;
    server->previous_layer = NULL;

    struct wlr_surface *surface = vela_view_surface(view);
    if (server->seat->keyboard_state.focused_surface == surface) {
        return;
    }
    // The active window is looked up among our live windows: one just closed
    // is already out of the list, and nothing is sent to it.
    struct vela_view *previous = vela_views_focused(server);
    if (previous && previous != view) {
        vela_view_set_activated(previous, false);
    }
    vela_view_set_activated(view, true);
    vela_input_keyboard_enter(server->input, surface);
}

void vela_focus_new_view(struct vela_server *server, struct vela_view *view)
{
    vela_focus_view(server, view);
    if (!vela_view_is_system_prompt(view)) {
        struct vela_view *prompt = vela_views_system_prompt(server);
        if (prompt) {
            vela_focus_view(server, prompt);
        }
    }
}

void vela_focus_layer(struct vela_server *server, struct vela_layer_surface *layer)
{
    // Use the surface's state: the "map" event comes before the commit that
    // updates layer->mapped.
    if (!layer || !layer->wlr->surface->mapped || server->locked) {
        return;
    }
    // A panel opening while the polkit dialog waits doesn't take its
    // keyboard (another "overlay" surface does: it is newer and on top).
    struct vela_layer_surface *modal = vela_focus_modal_layer(server);
    if (modal && modal != layer && layer->wlr->current.layer != ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        return;
    }
    // The dialog takes the keyboard: a keyboard Move/Size in progress would
    // swallow its keys (the password, Esc).
    if (layer == modal) {
        vela_interact_cancel_keyboard(server);
    }
    // Like Windows: opening the Start menu "dims" the active window.
    struct vela_view *active = vela_views_focused(server);
    if (active) {
        vela_view_set_activated(active, false);
    }
    // A menu opened from a panel (such as the Start menu): once the menu
    // closes, the keyboard goes back to the panel.
    if (server->focused_layer && server->focused_layer != layer) {
        server->previous_layer = server->focused_layer;
    }
    server->focused_layer = layer;
    vela_input_keyboard_enter(server->input, layer->wlr->surface);
}

struct vela_layer_surface *vela_focus_modal_layer(struct vela_server *server)
{
    struct vela_layer_surface *layer;
    wl_list_for_each_reverse (layer, &server->layer_surfaces, link) {
        const struct wlr_layer_surface_v1 *wlr = layer->wlr;
        if (wlr->surface->mapped && wlr->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
            && wlr->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
            return layer;
        }
    }
    return NULL;
}

void vela_focus_refocus(struct vela_server *server)
{
    if (server->locked) {
        return; // the keyboard belongs to the lock screen
    }
    // The polkit dialog first, if there is one (after unlocking, or when a
    // panel under it closes).
    struct vela_layer_surface *modal = vela_focus_modal_layer(server);
    if (modal) {
        server->focused_layer = NULL;
        vela_focus_layer(server, modal);
        return;
    }
    server->focused_layer = NULL;
    struct vela_layer_surface *previous = server->previous_layer;
    server->previous_layer = NULL;
    if (previous && previous->wlr->surface->mapped && vela_layer_surface_wants_keyboard(previous)) {
        vela_focus_layer(server, previous);
        return;
    }
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->mapped && !view->minimized && vela_view_on_current_workspace(view)) {
            vela_focus_view(server, view);
            return;
        }
    }
    // No window on this desktop: the keyboard goes to nobody.
    struct vela_view *focused = vela_views_focused(server);
    if (focused) {
        vela_view_set_activated(focused, false);
    }
    wlr_seat_keyboard_notify_clear_focus(server->seat);
}

void vela_focus_forget_view(struct vela_server *server, struct vela_view *view, bool was_focused)
{
    vela_snap_forget(server, view);
    vela_interact_forget(server, view);
    // A window that goes away during Alt+Tab closes the switcher.
    vela_switcher_forget(server, view);
    if (server->grabbed == view) {
        vela_snap_end_zone(server, false);
        server->grabbed = NULL;
        server->cursor_mode = VELA_CURSOR_PASSTHROUGH;
    }
    if (was_focused && !server->focused_layer) {
        vela_focus_refocus(server);
    }
}

void vela_focus_layer_unmapped(struct vela_server *server, struct vela_layer_surface *layer)
{
    if (server->focused_layer == layer) {
        vela_focus_refocus(server);
    }
}

void vela_focus_forget_layer(struct vela_server *server, struct vela_layer_surface *layer)
{
    if (server->previous_layer == layer) {
        server->previous_layer = NULL;
    }
    if (server->focused_layer == layer) {
        vela_focus_refocus(server);
    }
}
