// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "activation.h"

#include "alloc/alloc.h"
#include "focus.h"
#include "listen.h"
#include "server.h"
#include "view.h"
#include "workspace.h"

#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_activation_v1.h>

static void handle_new_token(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, new_activation_token);
    struct wlr_xdg_activation_token_v1 *token = data;
    // wlroots validates a supplied input serial and source focus at commit,
    // but both fields are optional in the protocol. A token without them is
    // not evidence of user interaction. Remember approval at issuance: a
    // launcher may close its surface before the launched app uses the token.
    if (!server->locked && token->seat == server->seat && token->surface
        && token->surface == server->seat->keyboard_state.focused_surface) {
        token->data = server;
    }
}

static void handle_request_activation(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, request_activation);
    struct wlr_xdg_activation_v1_request_activate_event *event = data;
    struct vela_view *view;
    wl_list_for_each(view, &server->views, link)
    {
        if (vela_view_surface(view) == event->surface) {
            if (server->locked || !event->token || event->token->data != server) {
                vela_view_set_attention(view, true);
            } else {
                vela_focus_new_view(server, view);
            }
            return;
        }
    }
}

// wlroots discards unknown or expired token strings before emitting
// request_activate. Observe those requests without taking over validation.
static void observe_activation_request(
    void *data, enum wl_protocol_logger_type direction, const struct wl_protocol_logger_message *message)
{
    struct vela_server *server = data;
    if (direction != WL_PROTOCOL_LOGGER_REQUEST || message->arguments_count != 2
        || strcmp(message->message->name, "activate") != 0
        || strcmp(wl_resource_get_class(message->resource), "xdg_activation_v1") != 0) {
        return;
    }
    const union wl_argument *args = message->arguments;
    if (!args[0].s || !args[1].o || wlr_xdg_activation_v1_find_token(server->activation, args[0].s)) {
        return;
    }
    struct wlr_surface *surface = wlr_surface_from_resource((struct wl_resource *)args[1].o);
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (vela_view_surface(view) == surface) {
            vela_view_set_attention(view, true);
            return;
        }
    }
}

void vela_activation_request_x11(struct vela_view *view)
{
    struct vela_server *server = view->server;
    if (server->locked || !view->mapped || view->minimized || !vela_view_on_current_workspace(view)
        || server->focused_layer) {
        vela_view_set_attention(view, true);
        return;
    }
    struct vela_view *focused = vela_views_focused(server);
    // wlroots' X11 activation event carries no input serial or trustworthy
    // user-action token. Permit the foreground window and its dialogs, but
    // never unminimize, switch desktops or displace another app on its say-so.
    if (focused == view || (focused && vela_view_is_transient_of(view, focused))
        || (!focused && !server->seat->keyboard_state.focused_surface)) {
        vela_focus_new_view(server, view);
    } else {
        vela_view_set_attention(view, true);
    }
}

void vela_activation_init(struct vela_server *server)
{
    server->activation = VELA_MUST(wlr_xdg_activation_v1_create(server->display));
    server->activation_logger =
        VELA_MUST(wl_display_add_protocol_logger(server->display, observe_activation_request, server));
    vela_listen(&server->activation->events.new_token, &server->new_activation_token, handle_new_token);
    vela_listen(&server->activation->events.request_activate, &server->request_activation, handle_request_activation);
}

void vela_activation_finish(struct vela_server *server)
{
    vela_unlisten(&server->new_activation_token);
    vela_unlisten(&server->request_activation);
    wl_protocol_logger_destroy(server->activation_logger);
    server->activation_logger = NULL;
}
