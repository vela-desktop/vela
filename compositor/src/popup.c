// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "popup.h"

#include "alloc/alloc.h"
#include "layer.h"
#include "listen.h"
#include "output.h"
#include "scene/scene.h"
#include "view.h"

#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

struct vela_popup {
    struct wlr_xdg_popup *xdg;
    struct vela_owner *owner;
    struct vela_tree *tree; // origin: the popup surface's
    struct vela_surface_node *surface_node;

    struct wl_listener commit;
    struct wl_listener reposition;
    struct wl_listener new_popup;
    struct wl_listener destroy;
};

// The owner's output, relative to the origin of its root surface (as wlroots
// wants, popups of popups included).
static struct wlr_box constraint_box(const struct vela_popup *popup)
{
    struct vela_output *output = NULL;
    struct vela_tree *tree = NULL;
    switch (popup->owner->kind) {
    case VELA_OWNER_VIEW: {
        struct vela_view *view = (struct vela_view *)popup->owner;
        output = vela_view_output(view);
        tree = view->tree;
        break;
    }
    case VELA_OWNER_LAYER: {
        struct vela_layer_surface *layer = (struct vela_layer_surface *)popup->owner;
        output = vela_layer_surface_output(layer);
        tree = layer->tree;
        break;
    }
    }
    struct wlr_box box = output ? vela_output_box(output) : (struct wlr_box) { 0, 0, 1920, 1080 };
    box.x -= (int)lround(tree->node.x);
    box.y -= (int)lround(tree->node.y);
    return box;
}

static void unconstrain(struct vela_popup *popup)
{
    struct wlr_box box = constraint_box(popup);
    wlr_xdg_popup_unconstrain_from_box(popup->xdg, &box);
}

static void handle_commit(struct wl_listener *listener, void *data)
{
    struct vela_popup *popup = wl_container_of(listener, popup, commit);
    if (popup->xdg->base->initial_commit) {
        unconstrain(popup); // also sends the first configure
    }
    // The position depends on its geometry and its parent's, which change with
    // commits.
    double x = 0.0;
    double y = 0.0;
    vela_popup_position(popup->xdg, &x, &y);
    vela_node_set_position(&popup->tree->node, x, y);
}

static void handle_reposition(struct wl_listener *listener, void *data)
{
    struct vela_popup *popup = wl_container_of(listener, popup, reposition);
    unconstrain(popup);
}

static void handle_new_popup(struct wl_listener *listener, void *data)
{
    struct vela_popup *popup = wl_container_of(listener, popup, new_popup);
    vela_popup_create(data, popup->tree, popup->owner);
}

static void handle_destroy(struct wl_listener *listener, void *data)
{
    struct vela_popup *popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->reposition.link);
    wl_list_remove(&popup->new_popup.link);
    wl_list_remove(&popup->destroy.link);
    vela_node_destroy(&popup->surface_node->node);
    vela_node_destroy(&popup->tree->node);
    free(popup);
}

void vela_popup_create(struct wlr_xdg_popup *xdg, struct vela_tree *parent, struct vela_owner *owner)
{
    struct vela_popup *popup = VELA_NEW(struct vela_popup);
    popup->xdg = xdg;
    popup->owner = owner;
    popup->tree = vela_tree_create(parent);
    popup->surface_node = vela_surface_node_create(popup->tree, xdg->base->surface);
    popup->tree->node.unclipped = true; // a menu goes outside the window: no window corners

    vela_listen(&xdg->base->surface->events.commit, &popup->commit, handle_commit);
    vela_listen(&xdg->events.reposition, &popup->reposition, handle_reposition);
    vela_listen(&xdg->base->events.new_popup, &popup->new_popup, handle_new_popup);
    vela_listen(&xdg->events.destroy, &popup->destroy, handle_destroy);
}
