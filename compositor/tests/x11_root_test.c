// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Xwayland's own xdg-output (x11_root.c, docs/renderer.md §3.9), without a
// GPU or Xwayland: headless wlroots outputs, a client standing for Xwayland
// and an ordinary one, in this process. Xwayland sees only our manager, with
// the outputs in pixels; the other client only wlroots' one. Layout changes
// are sent once, outputs that go away are let go.
#undef NDEBUG
#include "server.h"
#include "x11_root.h"

#include <assert.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/xwayland.h>
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/shell.h>

#include "xdg-output-unstable-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_event_loop *loop;

// ---------------------------------------------------------------- client --

struct output {
    struct wl_output *wl;
    struct zxdg_output_v1 *xdg;
    char name[32];
    int x, y, width, height;
    int done; // how many times the geometry was sent
};

struct client {
    struct wl_display *wl;
    struct wl_registry *registry;
    struct zxdg_output_manager_v1 *manager;
    uint32_t manager_version;
    int managers; // zxdg_output_manager_v1 globals seen
    bool shell; // the stand-in for xwayland_shell_v1
    struct output outputs[4];
    int output_count;
};

static void handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
    uint32_t version)
{
    struct client *c = data;
    if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        ++c->managers;
        c->manager_version = version;
        c->manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, version);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && c->output_count < 4) {
        c->outputs[c->output_count++].wl = wl_registry_bind(registry, name, &wl_output_interface, 3);
    } else if (strcmp(interface, "wl_seat") == 0) {
        c->shell = true;
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = { handle_global, handle_global_remove };

static void handle_position(void *data, struct zxdg_output_v1 *xdg, int32_t x, int32_t y)
{
    struct output *o = data;
    o->x = x;
    o->y = y;
}

static void handle_size(void *data, struct zxdg_output_v1 *xdg, int32_t width, int32_t height)
{
    struct output *o = data;
    o->width = width;
    o->height = height;
}

static void handle_done(void *data, struct zxdg_output_v1 *xdg)
{
    ++((struct output *)data)->done;
}

static void handle_name(void *data, struct zxdg_output_v1 *xdg, const char *name)
{
    struct output *o = data;
    strncpy(o->name, name, sizeof(o->name) - 1);
}

static void handle_description(void *data, struct zxdg_output_v1 *xdg, const char *description)
{
}

static const struct zxdg_output_v1_listener xdg_listener = {
    handle_position, handle_size, handle_done, handle_name, handle_description,
};

// Both ends in one thread: the server runs whenever the client waits.
static void pump(struct client *c)
{
    for (int i = 0; i < 10; ++i) {
        wl_display_flush(c->wl);
        wl_event_loop_dispatch(loop, 0);
        wl_display_flush_clients(display);
        while (wl_display_prepare_read(c->wl) != 0) {
            wl_display_dispatch_pending(c->wl);
        }
        struct pollfd fd = { wl_display_get_fd(c->wl), POLLIN, 0 };
        if (poll(&fd, 1, 5) > 0) {
            wl_display_read_events(c->wl);
        } else {
            wl_display_cancel_read(c->wl);
        }
        wl_display_dispatch_pending(c->wl);
    }
}

static struct wl_client *connect_client(struct client *c)
{
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    struct wl_client *server_side = wl_client_create(display, fds[0]);
    assert(server_side);
    memset(c, 0, sizeof(*c));
    c->wl = wl_display_connect_to_fd(fds[1]);
    assert(c->wl);
    return server_side;
}

static void list_globals(struct client *c)
{
    c->registry = wl_display_get_registry(c->wl);
    wl_registry_add_listener(c->registry, &registry_listener, c);
    pump(c);
}

static struct output *named(struct client *c, const char *name)
{
    for (int i = 0; i < c->output_count; ++i) {
        if (strcmp(c->outputs[i].name, name) == 0) {
            return &c->outputs[i];
        }
    }
    abort();
}

// ---------------------------------------------------------------- server --

static void bind_nothing(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
}

static void set_scale(struct wlr_output *output, float scale)
{
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_scale(&state, scale);
    assert(wlr_output_commit_state(output, &state));
    wlr_output_state_finish(&state);
}

int main(void)
{
    display = wl_display_create();
    loop = wl_display_get_event_loop(display);
    struct wlr_backend *backend = wlr_headless_backend_create(loop);
    assert(backend);

    // 1080p at 100% above 1440p at 125%.
    struct vela_server server = { 0 };
    server.display = display;
    server.output_layout = wlr_output_layout_create(display);
    server.xdg_outputs = wlr_xdg_output_manager_v1_create(display, server.output_layout);
    struct wlr_output *top = wlr_headless_add_output(backend, 1920, 1080);
    struct wlr_output *bottom = wlr_headless_add_output(backend, 2560, 1440);
    set_scale(bottom, 1.25f);
    wlr_output_layout_add(server.output_layout, top, 0, 0);
    wlr_output_layout_add(server.output_layout, bottom, 0, 1080);
    wlr_output_create_global(top, display);
    wlr_output_create_global(bottom, display);

    // Xwayland's client, and its shell's global.
    struct client x, other;
    struct wlr_xwayland_server xwayland_server = { .client = connect_client(&x) };
    struct wlr_xwayland_shell_v1 shell = { .global = wl_global_create(display, &wl_seat_interface, 1, NULL,
                                               bind_nothing) };
    struct wlr_xwayland xwayland = { .server = &xwayland_server, .shell_v1 = &shell };
    server.xwayland = &xwayland;
    connect_client(&other);

    assert(vela_x11_root_dpi(NULL) == 96);
    assert(!vela_x11_root_output(NULL, top) && !vela_x11_root_output_at(NULL, 0, 0));
    vela_x11_root_destroy(NULL);
    struct vela_x11_root *root = vela_x11_root_create(&server);
    assert(root);

    // Each client sees one xdg-output manager: Xwayland ours (version 2),
    // the other wlroots' (version 3). The shell is Xwayland's alone.
    list_globals(&x);
    list_globals(&other);
    assert(x.managers == 1 && x.manager_version == 2 && x.shell);
    assert(other.managers == 1 && other.manager_version == 3 && !other.shell);
    assert(x.output_count == 2 && other.output_count == 2);

    for (int i = 0; i < x.output_count; ++i) {
        struct output *o = &x.outputs[i];
        o->xdg = zxdg_output_manager_v1_get_xdg_output(x.manager, o->wl);
        zxdg_output_v1_add_listener(o->xdg, &xdg_listener, o);
    }
    pump(&x);
    struct output *x_top = named(&x, top->name);
    struct output *x_bottom = named(&x, bottom->name);
    assert(x_top->x == 0 && x_top->y == 0 && x_top->width == 1920 && x_top->height == 1080);
    assert(x_bottom->x == 0 && x_bottom->y == 1350 && x_bottom->width == 2560 && x_bottom->height == 1440);
    assert(x_top->done == 1 && x_bottom->done == 1);

    const struct vela_x11_output *in_root = vela_x11_root_output(root, bottom);
    assert(in_root && in_root->root_y == 1350 && in_root->scale == 1.25);
    assert(vela_x11_root_output_at(root, 100, 2000) == in_root);
    assert(vela_x11_root_dpi(root) == 120);

    // At 200% the top one has the largest density: the root grows below it.
    // The top output keeps its pixels, so only the bottom one hears of it.
    set_scale(top, 2.0f);
    pump(&x);
    assert(x_bottom->y == 2160 && x_bottom->done == 2);
    assert(x_top->width == 1920 && x_top->done == 1);
    assert(vela_x11_root_dpi(root) == 192);

    // Out of the layout: no longer in the root, nothing more sent for it.
    wlr_output_layout_remove(server.output_layout, top);
    pump(&x);
    assert(!vela_x11_root_output(root, top));
    assert(x_top->done == 1);
    assert(x_bottom->y == 0 && x_bottom->done == 3);

    // Unplugged, then released by Xwayland: its xdg-output is left alone.
    wlr_output_destroy(top);
    wl_output_release(x_top->wl);
    pump(&x);
    wlr_output_layout_add(server.output_layout, bottom, 10, 10);
    pump(&x);
    assert(x_top->done == 1 && x_bottom->done == 3); // the root origin stays (0, 0)
    zxdg_output_v1_destroy(x_top->xdg);
    pump(&x);

    // Gone with Xwayland still connected: its resources stay, inert.
    vela_x11_root_destroy(root);
    zxdg_output_v1_destroy(x_bottom->xdg);
    zxdg_output_manager_v1_destroy(x.manager);
    pump(&x);

    wl_output_release(x_bottom->wl);
    for (int i = 0; i < other.output_count; ++i) {
        wl_output_release(other.outputs[i].wl);
    }
    zxdg_output_manager_v1_destroy(other.manager);
    wl_registry_destroy(x.registry);
    wl_registry_destroy(other.registry);
    wl_display_disconnect(x.wl);
    wl_display_disconnect(other.wl);
    wl_display_destroy_clients(display);
    wl_global_destroy(shell.global);
    wlr_output_layout_destroy(server.output_layout);
    wlr_backend_destroy(backend);
    wl_display_destroy(display);
    return 0;
}
