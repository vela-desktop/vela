// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Per-client xdg-output scaling equivalent to the first half of KWin's
// XWayland connection scaleOverride. Keep Vela's regular wlroots xdg-output
// global visible to every ordinary Wayland client. The replacement global is
// visible ONLY to the specific wl_client running integrated XWayland.
//
// IMPORTANT: this is an experimental protocol-only stage. The corresponding
// wl_surface size, damage, input and XWM geometry transformations are not
// implemented yet. Do not activate it by default: changing just the output
// geometry would recreate the Steam input mismatch.
#include "x11_xdg_output.h"

#include "server.h"
#include "scene/scene.h"
#include "x11_coordinates.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/config.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#include <wlr/xwayland/server.h> // the actual Wayland client is owned by this server
#endif

#include "xdg-output-unstable-v1-protocol.h"

struct vela_x11_xdg_output {
    struct vela_server *server;
    struct wl_global *global;
    struct wl_global *wlroots_global;
    struct wl_list resources;
    struct wl_listener layout_change;
};

struct x11_output_resource {
    struct wl_list link;
    struct vela_x11_xdg_output *manager;
    struct wl_resource *resource;
    struct wl_resource *wl_output_resource; // NULL as soon as the client releases wl_output
    struct wlr_output *output; // NULL after hardware unplug, even if resource persists
    struct wl_listener wl_output_destroy;
    struct wl_listener output_destroy;
};

static bool is_xwayland_client(const struct vela_server *server, const struct wl_client *client)
{
#if WLR_HAS_XWAYLAND
    return server->xwayland && server->xwayland->server
        && server->xwayland->server->client == client;
#else
    (void)server;
    (void)client;
    return false;
#endif
}

static bool filter_global(const struct wl_client *client,
    const struct wl_global *global, void *data)
{
    struct vela_x11_xdg_output *manager = data;
    const bool xwayland = is_xwayland_client(manager->server, client);
    if (global == manager->global) {
        return xwayland;
    }
    if (global == manager->wlroots_global) {
        return !xwayland;
    }
    return true;
}

static void send_geometry(struct x11_output_resource *output)
{
    struct wlr_output *wlr = output->output;
    if (!wlr || !output->wl_output_resource || !wlr->enabled) {
        return;
    }

    struct wlr_box box;
    wlr_output_layout_get_box(output->manager->server->output_layout, wlr, &box);
    if (box.width <= 0 || box.height <= 0) {
        return;
    }

    double scale = output->manager->server->xwayland_apps_scale;
    if (!vela_x11_scale_valid(scale)) {
        scale = 1.0;
    }

    struct vela_x11_rect logical = { box.x, box.y, box.width, box.height };
    struct vela_x11_rect x11 = vela_wayland_rect_to_x11(scale, logical);
    zxdg_output_v1_send_logical_position(output->resource,
        (int32_t)lround(x11.x), (int32_t)lround(x11.y));
    zxdg_output_v1_send_logical_size(output->resource,
        (int32_t)lround(x11.width), (int32_t)lround(x11.height));
    // We advertise protocol v2: xdg-output.done is valid and we don't need
    // the v3+ wl_output.done synchronization owned by wlroots.
    zxdg_output_v1_send_done(output->resource);
}

static void wl_output_destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    struct x11_output_resource *output = wl_container_of(listener, output, wl_output_destroy);
    output->wl_output_resource = NULL;
    wl_list_remove(&output->wl_output_destroy.link);
    wl_list_init(&output->wl_output_destroy.link);
}

static void wlr_output_destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    struct x11_output_resource *output = wl_container_of(listener, output, output_destroy);
    output->output = NULL;
    wl_list_remove(&output->output_destroy.link);
    wl_list_init(&output->output_destroy.link);
}

static void output_resource_destroy(struct wl_resource *resource)
{
    struct x11_output_resource *output = wl_resource_get_user_data(resource);
    if (!output) return;
    wl_list_remove(&output->wl_output_destroy.link);
    wl_list_remove(&output->output_destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void output_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface output_impl = {
    .destroy = output_destroy,
};

static void get_xdg_output(struct wl_client *client, struct wl_resource *manager_resource,
    uint32_t id, struct wl_resource *wl_output_resource)
{
    struct vela_x11_xdg_output *manager = wl_resource_get_user_data(manager_resource);
    struct wlr_output *wlr = wlr_output_from_resource(wl_output_resource);
    if (!wlr) {
        // zxdg_output_manager_v1 has no protocol error enum for a stale
        // wl_output. This is an internal compositor/client lifetime error,
        // not an invented xdg-output protocol error code.
        wl_client_post_implementation_error(client,
            "xdg-output requested for an invalid or destroyed wl_output");
        return;
    }

    struct x11_output_resource *output = calloc(1, sizeof(*output));
    if (!output) {
        wl_client_post_no_memory(client);
        return;
    }
    output->resource = wl_resource_create(client, &zxdg_output_v1_interface,
        wl_resource_get_version(manager_resource), id);
    if (!output->resource) {
        free(output);
        wl_client_post_no_memory(client);
        return;
    }
    output->manager = manager;
    output->wl_output_resource = wl_output_resource;
    output->output = wlr;
    output->output_destroy.notify = wlr_output_destroyed;
    wl_signal_add(&wlr->events.destroy, &output->output_destroy);
    output->wl_output_destroy.notify = wl_output_destroyed;
    wl_resource_add_destroy_listener(wl_output_resource, &output->wl_output_destroy);
    if (wl_resource_get_version(output->resource) >= 2) {
        zxdg_output_v1_send_name(output->resource, wlr->name ? wlr->name : "");
        zxdg_output_v1_send_description(output->resource,
            wlr->description ? wlr->description : "");
    }
    wl_resource_set_implementation(output->resource, &output_impl,
        output, output_resource_destroy);
    wl_list_insert(&manager->resources, &output->link);
    send_geometry(output);
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}

static const struct zxdg_output_manager_v1_interface manager_impl = {
    .destroy = manager_destroy,
    .get_xdg_output = get_xdg_output,
};

static void bind_manager(struct wl_client *client, void *data,
    uint32_t version, uint32_t id)
{
    struct vela_x11_xdg_output *manager = data;
    if (!is_xwayland_client(manager->server, client)) {
        return; // normally impossible: the wl_global filter rejects it
    }
    struct wl_resource *resource = wl_resource_create(client,
        &zxdg_output_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

static void layout_changed(struct wl_listener *listener, void *data)
{
    (void)data;
    struct vela_x11_xdg_output *manager =
        wl_container_of(listener, manager, layout_change);
    struct x11_output_resource *output;
    wl_list_for_each(output, &manager->resources, link) {
        send_geometry(output);
    }
}

struct vela_x11_xdg_output *vela_x11_xdg_output_create(struct vela_server *server)
{
    // This is deliberately a developer-only experiment: xdg-output
    // geometry alone is insufficient for native-resolution X11.
    const char *experiment = getenv("VELA_XWAYLAND_XDG_OUTPUT_EXPERIMENT");
    if (!experiment || strcmp(experiment, "1") != 0 ||
        !server->xdg_output_manager) {
        return NULL;
    }
    // Never activate an unverified output/buffer coordinate override in
    // the user's real desktop session. The experiment may be exercised in
    // isolated headless/nested sessions (including GPU CI); those cannot
    // corrupt the main session's physical outputs or leave a persistent
    // bottom/right dirty rectangle after a direct-scanout transition.
    if (server->session) {
        wlr_log(WLR_ERROR, "Refusing unsafe XWayland scale experiment in a real session; "
            "run it in a nested/headless GPU test session instead");
        return NULL;
    }

    struct vela_x11_xdg_output *manager = calloc(1, sizeof(*manager));
    if (!manager) return NULL;
    manager->server = server;
    manager->wlroots_global = server->xdg_output_manager->global;
    wl_list_init(&manager->resources);
    manager->global = wl_global_create(server->display,
        &zxdg_output_manager_v1_interface, 2, manager, bind_manager);
    if (!manager->global) {
        free(manager);
        return NULL;
    }
    manager->layout_change.notify = layout_changed;
    wl_signal_add(&server->output_layout->events.change, &manager->layout_change);
    wl_display_set_global_filter(server->display, filter_global, manager);
    server->scene->x11_scale_override = true;
    wlr_log(WLR_INFO, "Experimental KWin-style per-XWayland-client xdg-output adapter enabled; "
        "buffer scaling, hotplug and input still require GPU validation");
    return manager;
}

void vela_x11_xdg_output_destroy(struct vela_x11_xdg_output *manager)
{
    if (!manager) return;

    // Call after wl_display_destroy_clients(): resource destruction callbacks
    // must run while the manager is still alive.
    manager->server->scene->x11_scale_override = false;
    wl_display_set_global_filter(manager->server->display, NULL, NULL);
    wl_list_remove(&manager->layout_change.link);
    wl_global_destroy(manager->global);
    free(manager);
}
