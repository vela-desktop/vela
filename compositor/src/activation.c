// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "activation.h"

#include "alloc/alloc.h"
#include "focus.h"
#include "listen.h"
#include "server.h"
#include "view.h"
#include "workspace.h"

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
    if (server->locked || event->token->data != server) {
        return;
    }
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (vela_view_surface(view) == event->surface) {
            vela_focus_new_view(server, view);
            return;
        }
    }
}

void vela_activation_request_x11(struct vela_view *view)
{
    struct vela_server *server = view->server;
    if (server->locked || !view->mapped || view->minimized || !vela_view_on_current_workspace(view)
        || server->focused_layer) {
        return;
    }
    struct vela_view *focused = vela_views_focused(server);
    // wlroots' X11 activation event carries no input serial or trustworthy
    // user-action token. Permit the foreground window and its dialogs, but
    // never unminimize, switch desktops or displace another app on its say-so.
    if (focused == view || (focused && vela_view_is_transient_of(view, focused))
        || (!focused && !server->seat->keyboard_state.focused_surface)) {
        vela_focus_new_view(server, view);
    }
}

void vela_activation_init(struct vela_server *server)
{
    struct wlr_xdg_activation_v1 *activation = VELA_MUST(wlr_xdg_activation_v1_create(server->display));
    vela_listen(&activation->events.new_token, &server->new_activation_token, handle_new_token);
    vela_listen(&activation->events.request_activate, &server->request_activation, handle_request_activation);
}

void vela_activation_finish(struct vela_server *server)
{
    vela_unlisten(&server->new_activation_token);
    vela_unlisten(&server->request_activation);
}
