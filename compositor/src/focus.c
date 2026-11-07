// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "focus.h"

#include "input.h"
#include "interact.h"
#include "layer.h"
#include "output.h"
#include "scene/scene.h"
#include "server.h"
#include "snap.h"
#include "switcher.h"
#include "view.h"
#include "workspace.h"

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>

// A completed Super tap is routed to Qt, but Qt may need another event-loop
// turn to create/commit the Start surface. Expose the passive taskbar in the
// *current* frame instead of waiting for that map. Cancel the preview when a
// real shell layer accepts keyboard focus; never keep the game uncovered when
// the shell is missing or unresponsive.
static int start_preview_timeout(void *data)
{
    struct vela_server *server = data;
    if (server->start_preview) {
        server->start_preview = false;
        vela_fullscreen_restack(server);
    }
    return 0;
}

void vela_fullscreen_preview_start(struct vela_server *server)
{
    if (server->locked || server->focused_layer || vela_focus_modal_layer(server)) {
        return;
    }
    struct vela_view *active = vela_views_focused(server);
    if (!active || !active->fullscreen) {
        return;
    }
    if (active->tree->node.parent != server->layers.fullscreen) {
        // A second Super tap closes Start before it maps: cancel the preview
        // immediately rather than exposing the taskbar until the timeout.
        if (server->start_preview) {
            server->start_preview = false;
            vela_fullscreen_restack(server);
        }
        return;
    }
    if (!server->start_preview_timer) {
        server->start_preview_timer = wl_event_loop_add_timer(server->loop, start_preview_timeout, server);
        if (!server->start_preview_timer) {
            return;
        }
    }
    server->start_preview = true;
    vela_fullscreen_restack(server);
    // Fallback only: a real mapped Start menu takes over before this expires.
    wl_event_source_timer_update(server->start_preview_timer, 500);
}

// Fullscreen is a client/geometry state, not a permanent global Z-order.
// The most recently activated window *intersecting each output* determines
// whether fullscreen owns that output. A window focused on monitor B must not
// unexpectedly expose the taskbar over a game on monitor A.
static bool view_overlaps_output(struct vela_view *view, struct vela_output *output)
{
    struct wlr_box area = vela_output_box(output);
    struct wlr_box frame = vela_view_frame_box(view);
    struct wlr_box overlap;
    return wlr_box_intersection(&overlap, &area, &frame);
}

// A normal window taking foreground over a game must expose the other normal
// windows on that monitor too. Place the background game below the lowest
// regular window, but above Wine BELOW backdrops. Do not do this when Start
// alone opens over an otherwise foreground game: it should still be visible
// behind Start, with only the taskbar additionally exposed.
static void background_game_below_windows(struct vela_server *server, struct vela_view *game,
    struct vela_output *output)
{
    // Children are bottom-to-top. If the game is already below the
    // lowest eligible normal window, do nothing: a no-op reorder invalidates
    // the entire scene on every pointer-motion event during a drag.
    bool game_is_lower = false;
    struct vela_node *node;
    wl_list_for_each (node, &server->layers.windows->children, link) {
        if (node == &game->tree->node) {
            game_is_lower = true;
            continue;
        }
        struct vela_owner *owner = node->data;
        if (!owner || owner->kind != VELA_OWNER_VIEW) {
            continue;
        }
        struct vela_view *other = (struct vela_view *)owner;
        if (other->mapped && !other->minimized && !other->fullscreen && !vela_view_stays_below(other)
            && vela_view_on_current_workspace(other) && view_overlaps_output(other, output)) {
            if (!game_is_lower) {
                vela_node_place_below(&game->tree->node, node);
            }
            break;
        }
    }
}

void vela_fullscreen_restack(struct vela_server *server)
{
    if (server->locked) {
        return;
    }
    struct vela_output *output;
    bool layers_changed = false;
    wl_list_for_each (output, &server->outputs, link) {
        struct vela_view *front = NULL;
        struct vela_view *view;
        wl_list_for_each (view, &server->views, link) {
            if (view->mapped && !view->minimized && vela_view_on_current_workspace(view)
                && view->tree->node.parent != server->layers.windows_out
                && !vela_view_stays_below(view) && view_overlaps_output(view, output)) {
                // Security prompts must remain visually accessible even if a
                // fullscreen app is activated while the prompt is mapped.
                bool is_prompt = vela_view_is_system_prompt(view);
                if (!front || is_prompt) {
                    front = view;
                }
                if (is_prompt) {
                    break;
                }
            }
        }

        // Start, Run, Quick Settings, etc. are TOP keyboard-interactive
        // surfaces. While one is mapped on this output, the passive TOP
        // taskbar should become visible as well.
        struct vela_layer_surface *panel = server->focused_layer;
        bool shell_above = (panel && panel->wlr->surface->mapped
            && vela_layer_surface_output(panel) == output
            && (panel->wlr->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP
                || panel->wlr->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY))
            || (server->start_preview && front && front->fullscreen
                && vela_views_focused(server) == front && vela_view_output(front) == output);

        wl_list_for_each (view, &server->views, link) {
            if (!view->mapped || !view->fullscreen
                || view->tree->node.parent == server->layers.windows_out
                || vela_view_output(view) != output) {
                continue;
            }
            // Wine's BELOW fullscreen backdrops must never be promoted.
            bool foreground = !shell_above && !vela_view_stays_below(view)
                && (front == view || (front && vela_view_is_transient_of(front, view)));
            struct vela_tree *target = foreground ? server->layers.fullscreen : server->layers.windows;
            if (view->tree->node.parent != target) {
                vela_node_reparent(&view->tree->node, target);
                layers_changed = true;
            }
            if (vela_view_stays_below(view)) {
                vela_view_keep_below(view);
            }
        }

        // Only synchronize all transient parents when a fullscreen view
        // actually switches scene layer, not on every drag motion.
        if (layers_changed) {
            vela_views_sync_transient_layers(server);
            layers_changed = false;
        }
        if (front && !front->fullscreen) {
            wl_list_for_each (view, &server->views, link) {
                if (view->mapped && view->fullscreen && !vela_view_stays_below(view)
                    && view->tree->node.parent == server->layers.windows
                    && vela_view_output(view) == output
                    && !vela_view_is_transient_of(front, view)) {
                    background_game_below_windows(server, view, output);
                }
            }
        }
        if (front && front->tree->node.parent == server->layers.windows) {
            vela_node_raise_to_top(&front->tree->node);
            vela_views_raise_transients(front);
        } else if (front && front->tree->node.parent == server->layers.fullscreen) {
            vela_node_raise_to_top(&front->tree->node);
            vela_views_raise_transients(front);
        }
    }
}

void vela_focus_view(struct vela_server *server, struct vela_view *view)
{
    if (!view || !view->mapped || server->locked || vela_view_stays_below(view)) {
        return;
    }
    // Any explicit activation cancels the speculative taskbar reveal.
    server->start_preview = false;
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
    vela_node_raise_to_top(&view->tree->node);
    vela_views_raise_transients(view);
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);
    vela_fullscreen_restack(server);
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
    // The previously focused TOP panel no longer masks the fullscreen game.
    // The first restack above intentionally still handled modal early returns.
    if (layer) {
        vela_fullscreen_restack(server);
    }

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
    // The real shell surface is now mapped: its normal layer/focus policy
    // owns the taskbar, with no speculative deadline.
    server->start_preview = false;
    vela_fullscreen_restack(server);
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
        if (view->mapped && !view->minimized && vela_view_on_current_workspace(view) && !vela_view_stays_below(view)) {
            vela_focus_view(server, view);
            return;
        }
    }
    vela_fullscreen_restack(server);
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
    vela_fullscreen_restack(server);
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
