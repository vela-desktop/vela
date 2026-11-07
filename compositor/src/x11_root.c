// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11_root.h"

#include "listen.h"
#include "server.h"
#include "util.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/config.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/shell.h>
#endif

#include "xdg-output-unstable-v1-protocol.h"

// Version 2: name and description, then done after every change. From
// version 3 done would be wl_output's, which wlroots sends for its own
// xdg-output.
#define XDG_OUTPUT_VERSION 2

struct vela_x11_root {
    struct vela_server *server;
    struct wl_global *global;
    struct wl_list resources; // xdg_output.link
    struct vela_x11_output *outputs;
    struct wlr_output **wlr; // the output of each, same index
    int count, capacity, wlr_capacity;
    struct wl_listener layout_change;
};

// One zxdg_output_v1 of Xwayland.
struct xdg_output {
    struct wl_list link;
    struct vela_x11_root *root;
    struct wl_resource *resource;
    struct wl_resource *wl_output; // NULL once Xwayland releases it
    struct wl_listener wl_output_destroy;
    bool sent;
    struct wlr_box box; // the last one sent
};

static bool is_xwayland(const struct vela_server *server, const struct wl_client *client)
{
#if WLR_HAS_XWAYLAND
    return server->xwayland && server->xwayland->server && server->xwayland->server->client == client;
#else
    return false;
#endif
}

// Xwayland sees our xdg-output and not wlroots'; the others the opposite.
// Its shell is Xwayland's alone too (wlroots asks for this filter).
static bool filter_global(const struct wl_client *client, const struct wl_global *global, void *data)
{
    struct vela_x11_root *root = data;
    struct vela_server *server = root->server;
    bool xwayland = is_xwayland(server, client);
    if (global == root->global) {
        return xwayland;
    }
    if (server->xdg_outputs && global == server->xdg_outputs->global) {
        return !xwayland;
    }
#if WLR_HAS_XWAYLAND
    if (server->xwayland && server->xwayland->shell_v1 && global == server->xwayland->shell_v1->global) {
        return xwayland;
    }
#endif
    return true;
}

const struct vela_x11_output *vela_x11_root_output(const struct vela_x11_root *root, const struct wlr_output *output)
{
    for (int i = 0; root && i < root->count; ++i) {
        if (root->wlr[i] == output) {
            return &root->outputs[i];
        }
    }
    return NULL;
}

const struct vela_x11_output *vela_x11_root_output_at(const struct vela_x11_root *root, double x, double y)
{
    return root ? vela_x11_output_at(root->outputs, (size_t)root->count, x, y) : NULL;
}

int vela_x11_root_dpi(const struct vela_x11_root *root)
{
    return root ? vela_x11_dpi(root->outputs, (size_t)root->count) : 96;
}

static void send_geometry(struct xdg_output *xdg)
{
    struct wlr_output *wlr = xdg->wl_output ? wlr_output_from_resource(xdg->wl_output) : NULL;
    const struct vela_x11_output *o = wlr ? vela_x11_root_output(xdg->root, wlr) : NULL;
    if (!o) {
        return; // not in the layout: Xwayland ignores it as with wlroots' xdg-output
    }
    struct wlr_box box = { o->root_x, o->root_y, o->pixel_width, o->pixel_height };
    if (xdg->sent && wlr_box_equal(&box, &xdg->box)) {
        return;
    }
    zxdg_output_v1_send_logical_position(xdg->resource, box.x, box.y);
    zxdg_output_v1_send_logical_size(xdg->resource, box.width, box.height);
    zxdg_output_v1_send_done(xdg->resource);
    xdg->sent = true;
    xdg->box = box;
}

static void update(struct vela_x11_root *root)
{
    struct wlr_output_layout *layout = root->server->output_layout;
    root->count = 0;
    struct wlr_output_layout_output *l;
    wl_list_for_each (l, &layout->outputs, link) {
        struct wlr_box box;
        wlr_output_layout_get_box(layout, l->output, &box);
        if (wlr_box_empty(&box)) {
            continue;
        }
        root->outputs = vela_grow(root->outputs, &root->capacity, root->count + 1, sizeof(*root->outputs));
        root->wlr = vela_grow(root->wlr, &root->wlr_capacity, root->count + 1, sizeof(*root->wlr));
        struct vela_x11_output *o = &root->outputs[root->count];
        *o = (struct vela_x11_output) { box.x, box.y, box.width, box.height, l->output->scale };
        wlr_output_transformed_resolution(l->output, &o->pixel_width, &o->pixel_height);
        root->wlr[root->count++] = l->output;
    }
    vela_x11_place(root->outputs, (size_t)root->count);
    struct xdg_output *xdg;
    wl_list_for_each (xdg, &root->resources, link) {
        send_geometry(xdg);
    }
}

static void handle_layout_change(struct wl_listener *listener, void *data)
{
    struct vela_x11_root *root = wl_container_of(listener, root, layout_change);
    update(root);
}

static void handle_wl_output_destroy(struct wl_listener *listener, void *data)
{
    struct xdg_output *xdg = wl_container_of(listener, xdg, wl_output_destroy);
    xdg->wl_output = NULL;
    vela_unlisten(&xdg->wl_output_destroy);
}

static void xdg_output_resource_destroy(struct wl_resource *resource)
{
    struct xdg_output *xdg = wl_resource_get_user_data(resource);
    vela_unlisten(&xdg->wl_output_destroy);
    wl_list_remove(&xdg->link);
    free(xdg);
}

static void handle_xdg_output_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface xdg_output_impl = {
    .destroy = handle_xdg_output_destroy,
};

static void handle_get_xdg_output(struct wl_client *client, struct wl_resource *manager, uint32_t id,
    struct wl_resource *wl_output)
{
    struct vela_x11_root *root = wl_resource_get_user_data(manager);
    struct xdg_output *xdg = calloc(1, sizeof(*xdg));
    if (!xdg) {
        wl_client_post_no_memory(client);
        return;
    }
    xdg->resource = wl_resource_create(client, &zxdg_output_v1_interface, wl_resource_get_version(manager), id);
    if (!xdg->resource) {
        free(xdg);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(xdg->resource, &xdg_output_impl, xdg, xdg_output_resource_destroy);
    xdg->root = root;
    xdg->wl_output = wl_output;
    wl_list_insert(&root->resources, &xdg->link);
    xdg->wl_output_destroy.notify = handle_wl_output_destroy;
    wl_resource_add_destroy_listener(wl_output, &xdg->wl_output_destroy);
    struct wlr_output *wlr = wlr_output_from_resource(wl_output);
    if (wlr && wl_resource_get_version(xdg->resource) >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
        zxdg_output_v1_send_name(xdg->resource, wlr->name);
        zxdg_output_v1_send_description(xdg->resource, wlr->description ? wlr->description : "");
    }
    send_geometry(xdg);
}

static void handle_manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy(resource);
}

static const struct zxdg_output_manager_v1_interface manager_impl = {
    .destroy = handle_manager_destroy,
    .get_xdg_output = handle_get_xdg_output,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client, &zxdg_output_manager_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

struct vela_x11_root *vela_x11_root_create(struct vela_server *server)
{
    struct vela_x11_root *root = calloc(1, sizeof(*root));
    if (!root) {
        return NULL;
    }
    root->server = server;
    wl_list_init(&root->resources);
    root->global = wl_global_create(server->display, &zxdg_output_manager_v1_interface, XDG_OUTPUT_VERSION, root,
        bind_manager);
    if (!root->global) {
        free(root);
        return NULL;
    }
    wl_display_set_global_filter(server->display, filter_global, root);
    vela_listen(&server->output_layout->events.change, &root->layout_change, handle_layout_change);
    update(root);
    return root;
}

void vela_x11_root_destroy(struct vela_x11_root *root)
{
    if (!root) {
        return;
    }
    wl_display_set_global_filter(root->server->display, NULL, NULL);
    vela_unlisten(&root->layout_change);
    // Resources of a client still connected outlive the global, inert.
    struct xdg_output *xdg, *tmp;
    wl_list_for_each_safe (xdg, tmp, &root->resources, link) {
        vela_unlisten(&xdg->wl_output_destroy);
        wl_resource_set_user_data(xdg->resource, NULL);
        wl_resource_set_destructor(xdg->resource, NULL);
        wl_list_remove(&xdg->link);
        free(xdg);
    }
    wl_global_destroy(root->global);
    free(root->outputs);
    free(root->wlr);
    free(root);
}
