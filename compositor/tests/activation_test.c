// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Real xdg-activation requests and wlroots serial validation over private
// sockets. Only the final focus change and view metadata are stubbed: no GPU.
#undef NDEBUG
#include "activation.h"
#include "focus.h"
#include "server.h"
#include "view.h"
#include "workspace.h"

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <wayland-client.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_seat.h>

#include "xdg-activation-v1-client-protocol.h"

static struct vela_server server;
static struct vela_view views[3];
static struct wlr_surface *surfaces[2];
static struct vela_view *focused;
static struct vela_view *last_activated;
static unsigned activations;

void vela_focus_new_view(struct vela_server *s, struct vela_view *view)
{
    assert(s == &server);
    last_activated = view;
    ++activations;
}

struct vela_view *vela_views_focused(struct vela_server *s)
{
    assert(s == &server);
    return focused;
}

struct wlr_surface *vela_view_surface(const struct vela_view *view)
{
    assert(view == &views[0] || view == &views[1]);
    return surfaces[view == &views[1]];
}

bool vela_view_on_current_workspace(const struct vela_view *view)
{
    return view->workspace == 0;
}

bool vela_view_is_transient_of(struct vela_view *view, struct vela_view *parent)
{
    return view == &views[2] && parent == &views[0];
}

struct client {
    struct wl_display *display;
    struct wl_client *server_side;
    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct wl_surface *surface;
    struct xdg_activation_v1 *activation;
    char token[128];
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_capabilities };

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
    uint32_t version)
{
    struct client *client = data;
    if (!strcmp(interface, wl_compositor_interface.name)) {
        client->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        client->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(client->seat, &seat_listener, NULL);
    } else if (!strcmp(interface, xdg_activation_v1_interface.name)) {
        client->activation = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
    }
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = { global, global_remove };

static void pump(struct client *client)
{
    for (int i = 0; i < 10; ++i) {
        assert(wl_display_flush(client->display) >= 0);
        assert(wl_event_loop_dispatch(server.loop, 0) >= 0);
        wl_display_flush_clients(server.display);
        while (wl_display_prepare_read(client->display) != 0) {
            assert(wl_display_dispatch_pending(client->display) >= 0);
        }
        struct pollfd fd = { wl_display_get_fd(client->display), POLLIN, 0 };
        if (poll(&fd, 1, 1) > 0) {
            assert(wl_display_read_events(client->display) >= 0);
        } else {
            wl_display_cancel_read(client->display);
        }
        assert(wl_display_dispatch_pending(client->display) >= 0);
    }
}

static void connect_client(struct client *client, int index)
{
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    client->server_side = wl_client_create(server.display, fds[0]);
    assert(client->server_side);
    client->display = wl_display_connect_to_fd(fds[1]);
    assert(client->display);
    struct wl_registry *registry = wl_display_get_registry(client->display);
    wl_registry_add_listener(registry, &registry_listener, client);
    pump(client);
    assert(client->compositor && client->seat && client->activation);
    client->surface = wl_compositor_create_surface(client->compositor);
    pump(client);
    surfaces[index] = wlr_surface_from_resource(wl_client_get_object(client->server_side,
        wl_proxy_get_id((struct wl_proxy *)client->surface)));
    assert(surfaces[index]);
    wl_registry_destroy(registry);
}

static uint32_t focus_client(struct client *client, int index)
{
    struct wlr_keyboard_modifiers modifiers = { 0 };
    wlr_seat_keyboard_notify_enter(server.seat, surfaces[index], NULL, 0, &modifiers);
    focused = &views[index];
    struct wlr_seat_client *seat_client = wlr_seat_client_for_wl_client(server.seat, client->server_side);
    assert(seat_client);
    return wlr_seat_client_next_serial(seat_client);
}

static void token_done(void *data, struct xdg_activation_token_v1 *token, const char *name)
{
    struct client *client = data;
    assert(strlen(name) < sizeof(client->token));
    strcpy(client->token, name);
}

static const struct xdg_activation_token_v1_listener token_listener = { .done = token_done };

static void request_token(struct client *client, bool with_surface, bool with_seat, uint32_t serial)
{
    client->token[0] = '\0';
    struct xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token(client->activation);
    xdg_activation_token_v1_add_listener(token, &token_listener, client);
    if (with_surface) {
        xdg_activation_token_v1_set_surface(token, client->surface);
    }
    if (with_seat) {
        xdg_activation_token_v1_set_serial(token, serial, client->seat);
    }
    xdg_activation_token_v1_commit(token);
    pump(client);
    assert(client->token[0]);
    xdg_activation_token_v1_destroy(token);
}

static void activate(struct client *client, const char *token, bool allowed)
{
    unsigned before = activations;
    xdg_activation_v1_activate(client->activation, token, client->surface);
    pump(client);
    assert(activations == before + (allowed ? 1u : 0u));
}

static void legacy_activation(void)
{
    focused = &views[0];
    views[0].fullscreen = true;
    unsigned before = activations;
    for (int i = 0; i < 5; ++i) {
        vela_activation_request_x11(&views[1]);
    }
    assert(activations == before); // repeated chat notifications over a game
    views[0].fullscreen = false;
    vela_activation_request_x11(&views[1]);
    assert(activations == before); // ordinary windows deserve the same protection
    vela_activation_request_x11(&views[0]);
    assert(activations == ++before && last_activated == &views[0]);
    vela_activation_request_x11(&views[2]);
    assert(activations == ++before && last_activated == &views[2]);

    views[2].minimized = true;
    vela_activation_request_x11(&views[2]);
    views[2].minimized = false;
    views[2].workspace = 1;
    vela_activation_request_x11(&views[2]);
    views[2].workspace = 0;
    views[2].mapped = false;
    vela_activation_request_x11(&views[2]);
    views[2].mapped = true;
    assert(activations == before);

    focused = NULL;
    server.focused_layer = (struct vela_layer_surface *)&server;
    vela_activation_request_x11(&views[1]);
    server.focused_layer = NULL;
    // A focused popup/other surface must also keep its keyboard.
    vela_activation_request_x11(&views[1]);
    assert(activations == before);
    wlr_seat_keyboard_notify_clear_focus(server.seat);
    vela_activation_request_x11(&views[1]);
    assert(activations == ++before);
    server.locked = true;
    vela_activation_request_x11(&views[1]);
    assert(activations == before);
    server.locked = false;
}

int main(void)
{
    server.display = wl_display_create();
    assert(server.display);
    server.loop = wl_display_get_event_loop(server.display);
    server.seat = wlr_seat_create(server.display, "seat0");
    assert(server.seat);
    assert(wlr_compositor_create(server.display, 6, NULL));
    wl_list_init(&server.views);
    for (int i = 0; i < 3; ++i) {
        views[i].server = &server;
        views[i].mapped = true;
        if (i < 2) {
            wl_list_insert(&server.views, &views[i].link);
        }
    }
    vela_activation_init(&server);
    struct client clients[2] = { 0 };
    connect_client(&clients[0], 0);
    connect_client(&clients[1], 1);
    uint32_t old_serial = focus_client(&clients[1], 1);
    uint32_t serial = focus_client(&clients[0], 0);

    // A background client can obtain protocol tokens while omitting the
    // optional seat/surface. None of these may raise its window.
    request_token(&clients[1], false, false, 0);
    activate(&clients[1], clients[1].token, false);
    request_token(&clients[1], true, false, 0);
    activate(&clients[1], clients[1].token, false);
    request_token(&clients[1], false, true, old_serial);
    activate(&clients[1], clients[1].token, false);
    request_token(&clients[1], true, true, old_serial);
    activate(&clients[1], clients[1].token, false);
    request_token(&clients[0], true, true, UINT32_MAX);
    activate(&clients[0], clients[0].token, false);

    // A real foreground grant can be passed to another client and survives
    // the launcher losing focus; its second use must fail.
    request_token(&clients[0], true, true, serial);
    focus_client(&clients[1], 1);
    activate(&clients[1], clients[0].token, true);
    assert(last_activated == &views[1]);
    activate(&clients[1], clients[0].token, false);

    // Neither issuing nor redeeming a token while locked may activate a view.
    serial = focus_client(&clients[0], 0);
    request_token(&clients[0], true, true, serial);
    server.locked = true;
    activate(&clients[1], clients[0].token, false);
    request_token(&clients[0], true, true, serial);
    server.locked = false;
    activate(&clients[1], clients[0].token, false);
    activate(&clients[1], "unknown-token", false);

    legacy_activation();

    // Closing the initiating surface after a legitimate launch must not
    // invalidate its already-approved, single-use grant.
    serial = focus_client(&clients[0], 0);
    request_token(&clients[0], true, true, serial);
    wl_surface_destroy(clients[0].surface);
    pump(&clients[0]);
    surfaces[0] = NULL;
    activate(&clients[1], clients[0].token, true);
    activate(&clients[1], clients[0].token, false);

    vela_activation_finish(&server);
    wl_display_disconnect(clients[0].display);
    wl_display_disconnect(clients[1].display);
    wl_display_destroy_clients(server.display);
    wl_display_destroy(server.display);
    puts("activation: background requests denied; foreground grants preserved");
    return 0;
}
