// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// X11 apps (Steam, many games, old apps) through Xwayland.
//
// Xwayland starts only when the first X11 app connects. Each of its "managed"
// windows becomes a vela_view like Wayland ones: same animations, snap,
// taskbar and Alt+Tab. "Override-redirect" windows (menus, tooltips,
// drop-downs) go where the app says, above everything, in a layer of their
// own.
//
// The X11 root is in pixels of the outputs (x11_root.h, docs/renderer.md
// §3.9): a game finds the native resolution and is drawn 1:1, and X11 apps
// scale themselves with Xft.dpi. Each window is in pixels of the output it is
// on: 1/scale logical units each (its density).

#include "view.h"

#include "interact.h"
#include "focus.h"
#include "bindings.h"
#include "input.h"
#include "listen.h"
#include "output.h"
#include "scene/scene.h"
#include "scene/surface.h"
#include "server.h"
#include "snap.h"
#include "switcher.h"
#include "workspace.h"
#include "x11_root.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/config.h>
#include <wlr/util/log.h>

#if WLR_HAS_XWAYLAND

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xcursor.h>
#include <xcb/xcbext.h>
#include <wlr/xwayland.h>
#include <wlr/xwayland/shell.h>

// Wine publishes the native HWND style as an X11 CARDINAL. Some games switch
// WS_POPUP -> WS_OVERLAPPEDWINDOW without sending EWMH fullscreen requests.
// Only an observed *transition* between those unmistakable styles is a hint,
// not an arbitrary change in location, dimensions, or opaque framebuffer data.
#define WINE_WS_POPUP 0x80000000u
#define WINE_WS_CAPTION 0x00c00000u

static bool wine_popup(uint32_t style)
{
    return (style & WINE_WS_POPUP) != 0 && (style & WINE_WS_CAPTION) != WINE_WS_CAPTION;
}

static bool wine_decorated(uint32_t style)
{
    return (style & WINE_WS_POPUP) == 0 && (style & WINE_WS_CAPTION) == WINE_WS_CAPTION;
}

// XCB property replies can wait for Xwayland. Never wait synchronously in
// wlroots' XWM handler or on the compositor's main loop.
static void request_wine_style(struct vela_view *view)
{
    struct vela_server *server = view->server;
    if (!server->xwayland_wine_style_atom || !server->xwayland_property_timer) {
        return;
    }
    if (view->x11_wine_style_pending) {
        // A second PropertyNotify arrived while a read was in flight. Ask
        // again after the response, so the final value is not overlooked.
        view->x11_wine_style_dirty = true;
        return;
    }
    xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(server->xwayland);
    if (!connection || xcb_connection_has_error(connection)) {
        return;
    }
    xcb_get_property_cookie_t cookie = xcb_get_property(connection, 0, view->x11->window_id,
        server->xwayland_wine_style_atom, XCB_ATOM_CARDINAL, 0, 1);
    view->x11_wine_style_cookie = cookie.sequence;
    view->x11_wine_style_pending = true;
    xcb_flush(connection);
    wl_event_source_timer_update(server->xwayland_property_timer, 1);
}

static void cancel_wine_style(struct vela_view *view)
{
    if (view->x11_wine_style_pending && view->server->xwayland) {
        xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(view->server->xwayland);
        if (connection) {
            xcb_discard_reply(connection, view->x11_wine_style_cookie);
        }
    }
    view->x11_wine_style_pending = false;
    view->x11_wine_style_dirty = false;
    view->x11_wine_style_known = false;
    view->x11_wine_restore_fullscreen = false;
}

// The state an app asks for before appearing (a game starting fullscreen)
// sits in _NET_WM_STATE before the window maps. wlroots reads that property
// only when it is told it changed, which it isn't when the app set it before
// wlroots watched the window, and when the window maps wlroots writes its own
// state over it. So it is read here at the MapRequest, which wlroots reports
// just before it maps the window: this read is answered before that write.
enum { NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_STATE_MAXIMIZED_VERT, NET_WM_STATE_MAXIMIZED_HORZ };

static void request_initial_state(struct vela_view *view)
{
    struct vela_server *server = view->server;
    xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(server->xwayland);
    if (!server->xwayland_state_atoms[NET_WM_STATE] || !server->xwayland_property_timer || !connection
        || xcb_connection_has_error(connection)) {
        return;
    }
    if (view->x11_initial_state_pending) {
        xcb_discard_reply(connection, view->x11_initial_state_cookie);
    }
    xcb_get_property_cookie_t cookie = xcb_get_property(connection, 0, view->x11->window_id,
        server->xwayland_state_atoms[NET_WM_STATE], XCB_ATOM_ATOM, 0, 32);
    view->x11_initial_state_cookie = cookie.sequence;
    view->x11_initial_state_pending = true;
    view->x11_start_fullscreen = false;
    view->x11_start_maximized = false;
    xcb_flush(connection);
    wl_event_source_timer_update(server->xwayland_property_timer, 1);
}

static void cancel_initial_state(struct vela_view *view)
{
    if (view->x11_initial_state_pending && view->server->xwayland) {
        xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(view->server->xwayland);
        if (connection) {
            xcb_discard_reply(connection, view->x11_initial_state_cookie);
        }
    }
    view->x11_initial_state_pending = false;
    view->x11_start_fullscreen = false;
    view->x11_start_maximized = false;
}

static void apply_initial_state(struct vela_view *view, const xcb_get_property_reply_t *reply)
{
    const uint32_t *atoms = view->server->xwayland_state_atoms;
    const xcb_atom_t *state = xcb_get_property_value(reply);
    int count = xcb_get_property_value_length(reply) / (int)sizeof(*state);
    bool vert = false;
    bool horz = false;
    for (int i = 0; i < count; i++) {
        view->x11_start_fullscreen |= state[i] == atoms[NET_WM_STATE_FULLSCREEN];
        vert |= state[i] == atoms[NET_WM_STATE_MAXIMIZED_VERT];
        horz |= state[i] == atoms[NET_WM_STATE_MAXIMIZED_HORZ];
    }
    view->x11_start_maximized = vert || horz;
    if (!view->mapped) {
        return; // handle_map applies it
    }
    if (view->x11_start_fullscreen && !view->fullscreen) {
        vela_view_set_fullscreen(view, true);
    } else if (view->x11_start_maximized && !view->maximized && !view->fullscreen) {
        vela_view_set_maximized(view, true, true);
    }
}

static void apply_wine_style(struct vela_view *view, uint32_t style)
{
    uint32_t previous = view->x11_wine_style;
    bool known = view->x11_wine_style_known;
    view->x11_wine_style = style;
    view->x11_wine_style_known = true;
    if (!known || previous == style) {
        return;
    }
    if (wine_popup(previous) && wine_decorated(style) && view->fullscreen) {
        wlr_log(WLR_INFO, "Xwayland: Wine popup -> decorated, exiting stale fullscreen for window 0x%x",
            (unsigned)view->x11->window_id);
        vela_view_set_fullscreen(view, false);
        view->x11_wine_restore_fullscreen = true;
    } else if (!wine_popup(previous) && wine_popup(style) && !view->fullscreen
        && view->x11_wine_restore_fullscreen) {
        // Wine may pass through a non-popup intermediate style before its
        // final WS_POPUP. Only undo a Wine fullscreen exit we initiated;
        // an arbitrary WS_POPUP does not request fullscreen.
        wlr_log(WLR_INFO, "Xwayland: Wine windowed -> popup, restoring fullscreen for window 0x%x",
            (unsigned)view->x11->window_id);
        vela_view_set_fullscreen(view, true);
    }
}

static void wake_window_manager(struct vela_server *server);

// Returns whether it is still waiting for the reply. Polling reads from the
// window manager's socket outside its event handler, so it can leave events in
// xcb's queue: whoever polls wakes the window manager afterwards.
static bool poll_initial_state(struct vela_view *view, xcb_connection_t *connection)
{
    xcb_get_property_reply_t *reply = NULL;
    xcb_generic_error_t *error = NULL;
    if (!xcb_poll_for_reply(connection, view->x11_initial_state_cookie, (void **)&reply, &error)) {
        return true;
    }
    view->x11_initial_state_pending = false;
    if (!error && reply && reply->type == XCB_ATOM_ATOM && reply->format == 32) {
        apply_initial_state(view, reply);
    }
    free(reply);
    free(error);
    return false;
}

static void publish_dpi(struct vela_server *server);

// The root's resources changed (xrdb): read them, without waiting.
static void request_resources(struct vela_server *server)
{
    xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(server->xwayland);
    if (!server->xwayland_property_timer || !connection || xcb_connection_has_error(connection)) {
        return;
    }
    if (server->xwayland_resources_pending) {
        xcb_discard_reply(connection, server->xwayland_resources_cookie);
    }
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    // Up to 256 KiB, in 32-bit units: more than any resource database.
    xcb_get_property_cookie_t cookie = xcb_get_property(connection, 0, screen->root, XCB_ATOM_RESOURCE_MANAGER,
        XCB_ATOM_STRING, 0, 65536);
    server->xwayland_resources_cookie = cookie.sequence;
    server->xwayland_resources_pending = true;
    xcb_flush(connection);
    wl_event_source_timer_update(server->xwayland_property_timer, 1);
}

// Returns whether it is still waiting. Whoever replaced the resources without
// Xft.dpi gets ours back; one they set themselves stays.
static bool poll_resources(struct vela_server *server, xcb_connection_t *connection)
{
    xcb_get_property_reply_t *reply = NULL;
    xcb_generic_error_t *error = NULL;
    if (!xcb_poll_for_reply(connection, server->xwayland_resources_cookie, (void **)&reply, &error)) {
        return true;
    }
    server->xwayland_resources_pending = false;
    // What doesn't fit in one read is left alone: appending again would not
    // be seen either.
    bool set = !error && reply && reply->format == 8
        && (reply->bytes_after > 0
            || vela_x11_resources_set_dpi(xcb_get_property_value(reply),
                (size_t)xcb_get_property_value_length(reply)));
    if (!error && !set) {
        server->xwayland_dpi = 0;
        publish_dpi(server);
    }
    free(reply);
    free(error);
    return false;
}

static int poll_property_replies(void *data)
{
    struct vela_server *server = data;
    xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(server->xwayland);
    if (!connection || xcb_connection_has_error(connection)) {
        return 0;
    }
    bool waiting = false;
    bool polled = false;
    if (server->xwayland_resources_pending) {
        waiting |= poll_resources(server, connection);
        polled = true;
    }
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->x11 && view->x11_initial_state_pending) {
            waiting |= poll_initial_state(view, connection);
            polled = true;
        }
        if (!view->x11 || !view->x11_wine_style_pending) {
            continue;
        }
        polled = true;
        xcb_get_property_reply_t *reply = NULL;
        xcb_generic_error_t *error = NULL;
        if (!xcb_poll_for_reply(connection, view->x11_wine_style_cookie, (void **)&reply, &error)) {
            waiting = true;
            continue;
        }
        view->x11_wine_style_pending = false;
        bool valid = !error && reply && reply->type == XCB_ATOM_CARDINAL
            && reply->format == 32 && xcb_get_property_value_length(reply) == sizeof(uint32_t);
        if (valid) {
            uint32_t style;
            memcpy(&style, xcb_get_property_value(reply), sizeof(style));
            apply_wine_style(view, style);
        } else {
            view->x11_wine_style_known = false;
            view->x11_wine_restore_fullscreen = false;
        }
        free(reply);
        free(error);
        if (view->x11_wine_style_dirty) {
            view->x11_wine_style_dirty = false;
            request_wine_style(view);
            waiting = true;
        }
    }
    if (waiting) {
        wl_event_source_timer_update(server->xwayland_property_timer, 5);
    }
    if (polled) {
        wake_window_manager(server);
    }
    return 0;
}

// Called by wlroots before its own XWM event processing; return false so it
// still sees and handles every PropertyNotify itself.
static bool handle_x11_event(struct wlr_xwayland *xwayland, xcb_generic_event_t *event)
{
    struct vela_server *server = xwayland->data;
    if ((event->response_type & 0x7f) != XCB_PROPERTY_NOTIFY) {
        return false;
    }
    const xcb_property_notify_event_t *property = (const xcb_property_notify_event_t *)event;
    if (property->atom == XCB_ATOM_RESOURCE_MANAGER) {
        request_resources(server);
        return false;
    }
    if (!server->xwayland_wine_style_atom || property->atom != server->xwayland_wine_style_atom) {
        return false;
    }
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->x11 && view->x11->window_id == property->window) {
            request_wine_style(view);
            break;
        }
    }
    return false;
}

// ------------------------------------------------------ root coordinates --

// From the layout to the root and back, through the output `o` (NULL: no root
// yet, the two coincide).
static void to_root(const struct vela_x11_output *o, double lx, double ly, double *x, double *y)
{
    if (o) {
        vela_x11_from_logical(o, lx, ly, x, y);
    } else {
        *x = lx;
        *y = ly;
    }
}

static void from_root(const struct vela_x11_output *o, double x, double y, double *lx, double *ly)
{
    if (o) {
        vela_x11_to_logical(o, x, y, lx, ly);
    } else {
        *lx = x;
        *ly = y;
    }
}

static double density_of(const struct vela_x11_output *o)
{
    return o ? o->scale : 1.0;
}

// The output in the root that holds a logical point, or else the nearest one.
static const struct vela_x11_output *root_output_at(struct vela_server *server, double lx, double ly)
{
    double x = lx;
    double y = ly;
    wlr_output_layout_closest_point(server->output_layout, NULL, lx, ly, &x, &y);
    struct vela_output *output = vela_output_at(server, x, y);
    return output ? vela_x11_root_output(server->x11_root, output->wlr) : NULL;
}

static double view_density(const struct vela_view *view)
{
    return view->x11_density > 0.0 ? view->x11_density : 1.0;
}

// The window passes onto an output with another scale: it keeps its logical
// size, so it gets as many pixels as it takes there.
static void set_view_density(struct vela_view *view, double density)
{
    if (view->x11_width <= 0.0 && view->x11->width > 0) {
        view->x11_width = view->x11->width / view_density(view);
        view->x11_height = view->x11->height / view_density(view);
    }
    view->x11_density = density;
    if (view->x11->surface) {
        vela_surface_set_density(view->x11->surface, density);
    }
}

// The output a window is on, from the center of its frame.
static const struct vela_x11_output *view_root_output(struct vela_view *view)
{
    struct wlr_box frame = vela_view_frame_box(view);
    return root_output_at(view->server, frame.x + frame.width / 2.0, frame.y + frame.height / 2.0);
}

// -------------------------------------------- menus, tooltips, drop-downs --

// An X11 menu, tooltip or drop-down: no window manager, the app chooses the
// position (in global coordinates).
struct unmanaged {
    struct vela_server *server;
    struct wlr_xwayland_surface *x11;
    struct vela_tree *tree;
    struct vela_surface_node *surface_node;
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener set_geometry;
    struct wl_listener request_configure;
    struct wl_listener set_override_redirect;
    struct wl_listener request_below;
    struct wl_listener destroy;
};

static struct vela_view *view_create(struct vela_server *server, struct wlr_xwayland_surface *x11);
static void handle_associate(struct wl_listener *listener, void *data);
static void handle_map(struct wl_listener *listener, void *data);

// A BELOW backdrop must render behind fullscreen windows even when Wine
// changes it to override-redirect while mapped. Ordinary menus stay on top.
static void unmanaged_restack(struct unmanaged *menu)
{
    struct vela_tree *layer = menu->x11->below
        ? menu->server->layers.windows : menu->server->layers.x11_popups;
    vela_node_reparent(&menu->tree->node, layer);
    if (menu->x11->below) {
        // The tree is already a child, so the list cannot be empty.
        struct vela_node *bottom = wl_container_of(layer->children.next, bottom, link);
        if (bottom != &menu->tree->node) {
            vela_node_place_below(&menu->tree->node, bottom);
        }
    } else {
        vela_node_raise_to_top(&menu->tree->node);
    }
}

// Where the app puts it, on the output holding its center; in pixels of that
// output.
static void unmanaged_place(struct unmanaged *menu)
{
    struct wlr_xwayland_surface *x11 = menu->x11;
    const struct vela_x11_output *o
        = vela_x11_root_output_at(menu->server->x11_root, x11->x + x11->width / 2.0, x11->y + x11->height / 2.0);
    double lx = 0.0;
    double ly = 0.0;
    from_root(o, x11->x, x11->y, &lx, &ly);
    vela_node_set_position(&menu->tree->node, lx, ly);
    if (x11->surface) {
        vela_surface_set_density(x11->surface, density_of(o));
    }
}

static void handle_unmanaged_map(struct wl_listener *listener, void *data)
{
    struct unmanaged *menu = wl_container_of(listener, menu, map);
    unmanaged_place(menu);
    unmanaged_restack(menu);
    vela_node_set_enabled(&menu->tree->node, true);
    // BELOW backdrops must not steal keyboard focus from the game.
    if (!menu->x11->below && wlr_xwayland_surface_override_redirect_wants_focus(menu->x11)) {
        vela_input_keyboard_enter(menu->server->input, menu->x11->surface);
    }
}

static void handle_unmanaged_request_below(struct wl_listener *listener, void *data)
{
    struct unmanaged *menu = wl_container_of(listener, menu, request_below);
    unmanaged_restack(menu);
    if (menu->x11->below && menu->x11->surface
        && menu->server->seat->keyboard_state.focused_surface == menu->x11->surface) {
        vela_focus_refocus(menu->server);
    }
}

static void handle_unmanaged_unmap(struct wl_listener *listener, void *data)
{
    struct unmanaged *menu = wl_container_of(listener, menu, unmap);
    vela_node_set_enabled(&menu->tree->node, false);
    if (menu->server->seat->keyboard_state.focused_surface == menu->x11->surface) {
        vela_focus_refocus(menu->server);
    }
}

// wlroots maps an X11 surface at the first commit with a buffer after the
// X window and the Wayland surface are paired. The pairing message can
// arrive after Xwayland has already committed the window's content; then
// no further commit comes (Xwayland waits for a frame callback that an
// unmapped surface never gets) and the window would never show.
static void map_if_drawn(struct wlr_surface *surface)
{
    if (!surface->mapped && wlr_surface_has_buffer(surface)) {
        wlr_surface_map(surface);
    }
}

static void handle_unmanaged_associate(struct wl_listener *listener, void *data)
{
    struct unmanaged *menu = wl_container_of(listener, menu, associate);
    menu->surface_node = vela_surface_node_create(menu->tree, menu->x11->surface);
    unmanaged_place(menu);
    vela_listen(&menu->x11->surface->events.map, &menu->map, handle_unmanaged_map);
    vela_listen(&menu->x11->surface->events.unmap, &menu->unmap, handle_unmanaged_unmap);
    map_if_drawn(menu->x11->surface);
}

static void handle_unmanaged_dissociate(struct wl_listener *listener, void *data)
{
    struct unmanaged *menu = wl_container_of(listener, menu, dissociate);
    vela_unlisten(&menu->map);
    vela_unlisten(&menu->unmap);
    if (menu->surface_node) {
        vela_node_destroy(&menu->surface_node->node);
        menu->surface_node = NULL;
    }
}

static void handle_unmanaged_set_geometry(struct wl_listener *listener, void *data)
{
    struct unmanaged *menu = wl_container_of(listener, menu, set_geometry);
    unmanaged_place(menu);
}

static void handle_unmanaged_request_configure(struct wl_listener *listener, void *data)
{
    struct unmanaged *menu = wl_container_of(listener, menu, request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    wlr_xwayland_surface_configure(menu->x11, event->x, event->y, event->width, event->height);
}

static void handle_unmanaged_destroy(struct wl_listener *listener, void *data)
{
    struct unmanaged *menu = wl_container_of(listener, menu, destroy);
    vela_unlisten(&menu->associate);
    vela_unlisten(&menu->dissociate);
    vela_unlisten(&menu->map);
    vela_unlisten(&menu->unmap);
    vela_unlisten(&menu->set_geometry);
    vela_unlisten(&menu->request_configure);
    vela_unlisten(&menu->set_override_redirect);
    vela_unlisten(&menu->request_below);
    vela_unlisten(&menu->destroy);
    if (menu->surface_node) {
        vela_node_destroy(&menu->surface_node->node);
    }
    vela_node_destroy(&menu->tree->node);
    free(menu);
}

// Wine creates some windows override-redirect and makes them managed later
// (its fullscreen backdrop), or back: the window changes kind, keeping its
// surface and its map state.
static void handle_unmanaged_set_override_redirect(struct wl_listener *listener, void *data)
{
    struct unmanaged *menu = wl_container_of(listener, menu, set_override_redirect);
    struct vela_server *server = menu->server;
    struct wlr_xwayland_surface *x11 = menu->x11;
    // Only promote a surface which really became managed. The notification
    // can also arrive when X11 reapplies the current window attributes.
    if (x11->override_redirect) {
        return;
    }
    bool mapped = x11->surface && x11->surface->mapped;
    if (mapped) {
        handle_unmanaged_unmap(&menu->unmap, NULL);
    }
    handle_unmanaged_destroy(&menu->destroy, NULL);
    struct vela_view *view = view_create(server, x11);
    if (x11->surface) {
        handle_associate(&view->associate, NULL);
        if (mapped) {
            handle_map(&view->map, NULL);
        }
    }
}

static struct unmanaged *unmanaged_create(struct vela_server *server, struct wlr_xwayland_surface *x11)
{
    struct unmanaged *menu = calloc(1, sizeof(*menu));
    menu->server = server;
    menu->x11 = x11;
    menu->tree = vela_tree_create(server->layers.x11_popups);
    vela_node_set_enabled(&menu->tree->node, false);
    wl_list_init(&menu->map.link);
    wl_list_init(&menu->unmap.link);
    vela_listen(&x11->events.associate, &menu->associate, handle_unmanaged_associate);
    vela_listen(&x11->events.dissociate, &menu->dissociate, handle_unmanaged_dissociate);
    vela_listen(&x11->events.set_geometry, &menu->set_geometry, handle_unmanaged_set_geometry);
    vela_listen(&x11->events.request_configure, &menu->request_configure, handle_unmanaged_request_configure);
    vela_listen(&x11->events.set_override_redirect, &menu->set_override_redirect,
        handle_unmanaged_set_override_redirect);
    vela_listen(&x11->events.request_below, &menu->request_below, handle_unmanaged_request_below);
    vela_listen(&x11->events.destroy, &menu->destroy, handle_unmanaged_destroy);
    return menu;
}

// ----------------------------------------------------------- X11 windows --

void vela_view_sync_x11(struct vela_view *view)
{
    struct wlr_xwayland_surface *x11 = view->x11;
    if (!x11 || !x11->surface) {
        return;
    }
    const struct vela_x11_output *o = view_root_output(view);
    if (density_of(o) != view_density(view)) {
        set_view_density(view, density_of(o));
    }
    double d = view_density(view);
    double x = 0.0;
    double y = 0.0;
    to_root(o, view->tree->node.x, view->tree->node.y, &x, &y);
    const struct wlr_box wanted = {
        .x = (int)lround(x),
        .y = (int)lround(y),
        .width = view->x11_width > 0.0 ? (int)lround(view->x11_width * d) : x11->width,
        .height = view->x11_height > 0.0 ? (int)lround(view->x11_height * d) : x11->height,
    };
    const struct wlr_box *sent = &view->x11_sent;
    if (wanted.width <= 0 || wanted.height <= 0
        || (wanted.x == sent->x && wanted.y == sent->y && wanted.width == sent->width
            && wanted.height == sent->height)) {
        return;
    }
    wlr_xwayland_surface_configure(x11, (int16_t)wanted.x, (int16_t)wanted.y, (uint16_t)wanted.width,
        (uint16_t)wanted.height);
    view->x11_sent = wanted;
}

static void handle_map_request(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, map_request);
    request_initial_state(view);
}

static void handle_map(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, map);
    xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(view->server->xwayland);
    if (view->x11_initial_state_pending && connection) {
        if (poll_initial_state(view, connection)) {
            // Not answered yet: the timer applies it when it is.
            wl_event_source_timer_update(view->server->xwayland_property_timer, 1);
        }
        wake_window_manager(view->server);
    }
    // It opens on the output under the cursor (vela_view_mapped), in its
    // pixels.
    struct vela_output *output = vela_output_under_cursor(view->server);
    const struct vela_x11_output *o = output ? vela_x11_root_output(view->server->x11_root, output->wlr) : NULL;
    view->x11_density = density_of(o);
    // The size it asked for before appearing is in those pixels, wherever
    // the window goes later.
    view->x11_initial_density = view->x11_density;
    vela_surface_set_density(view->x11->surface, view->x11_density);
    // Establish the baseline even when Wine set the style before mapping:
    // only later transitions may affect fullscreen policy.
    request_wine_style(view);
    vela_view_update_decoration(view);
    // X11 apps can ask to start maximized or fullscreen (games).
    if (view->x11->fullscreen || view->x11_start_fullscreen) {
        vela_view_set_fullscreen(view, true);
    } else if (view->x11->maximized_horz || view->x11->maximized_vert || view->x11_start_maximized) {
        vela_view_set_maximized(view, true, true);
    }
    vela_view_mapped(view);
}

static void handle_unmap(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, unmap);
    cancel_wine_style(view);
    cancel_initial_state(view);
    vela_view_unmapped(view);
}

static void handle_commit(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, commit);
    vela_view_committed(view);
}

// The Wayland surface comes later (associate) and can leave before the X11
// window (dissociate).
static void handle_associate(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, associate);
    struct wlr_surface *surface = view->x11->surface;
    view->surface_node = vela_surface_node_create(view->tree, surface);
    vela_surface_set_density(surface, view_density(view));
    vela_listen(&surface->events.map, &view->map, handle_map);
    vela_listen(&surface->events.unmap, &view->unmap, handle_unmap);
    vela_listen(&surface->events.commit, &view->commit, handle_commit);
    map_if_drawn(surface);
}

static void handle_dissociate(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, dissociate);
    cancel_wine_style(view);
    vela_unlisten(&view->map);
    vela_unlisten(&view->unmap);
    vela_unlisten(&view->commit);
    if (view->surface_node) {
        vela_node_destroy(&view->surface_node->node);
        view->surface_node = NULL;
    }
}

static void handle_destroy(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, destroy);
    cancel_wine_style(view);
    cancel_initial_state(view);
    vela_view_destroy(view);
}

static void handle_request_configure(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    if (!view->mapped) {
        // Before appearing the app chooses where and how large (remembered:
        // it's its "normal" size).
        view->x11_initial = (struct wlr_box) { event->x, event->y, event->width, event->height };
        wlr_xwayland_surface_configure(view->x11, event->x, event->y, event->width, event->height);
        return;
    }
    if (view->maximized || view->fullscreen || !vela_snap_is_none(view->snap)) {
        view->x11_sent = (struct wlr_box) { 0 }; // we decide: our geometry is repeated
        vela_view_sync_x11(view);
        return;
    }
    // A free window: the app can move and resize itself (games changing
    // resolution, dialogs centering themselves), but stays inside the output
    // it wants to go to: Steam, for example, goes back to the position and
    // size it remembers, even when they no longer fit. The request is in
    // pixels of the output holding its center.
    const struct vela_x11_output *o = vela_x11_root_output_at(view->server->x11_root,
        event->x + event->width / 2.0, event->y + event->height / 2.0);
    double d = density_of(o);
    double lx = 0.0;
    double ly = 0.0;
    from_root(o, event->x, event->y, &lx, &ly);
    double width = event->width / d;
    double height = event->height / d;
    int bar = vela_view_title_bar_height(view);
    struct wlr_box frame
        = { (int)lround(lx), (int)lround(ly) - bar, (int)lround(width), (int)lround(height) + bar };
    struct vela_output *output
        = vela_output_at(view->server, frame.x + frame.width / 2.0, frame.y + frame.height / 2.0);
    if (!output) {
        output = vela_view_output(view);
    }
    struct wlr_box fitted = output ? vela_output_fit(output, frame) : frame;
    // Where and how large it asked, exactly, unless it didn't fit.
    if (fitted.x != frame.x || fitted.y != frame.y) {
        lx = fitted.x;
        ly = fitted.y + bar;
    }
    if (fitted.width != frame.width || fitted.height != frame.height) {
        width = fitted.width;
        height = fitted.height - bar;
    }
    vela_node_set_position(&view->tree->node, lx, ly);
    view->x11_width = width;
    view->x11_height = height;
    if (d != view_density(view)) {
        view->x11_density = d;
        vela_surface_set_density(view->x11->surface, d);
    }
    vela_view_sync_x11(view);
}

static void handle_request_move(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_move);
    vela_interact_begin(view->server, view, VELA_CURSOR_MOVE, 0, false);
}

static void handle_request_resize(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_resize);
    struct wlr_xwayland_resize_event *event = data;
    vela_interact_begin(view->server, view, VELA_CURSOR_RESIZE, event->edges, false);
}

static void handle_request_maximize(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_maximize);
    vela_view_set_maximized(view, view->x11->maximized_horz || view->x11->maximized_vert, true);
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_fullscreen);
    vela_view_set_fullscreen(view, view->x11->fullscreen);
}

static void handle_request_minimize(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_minimize);
    struct wlr_xwayland_minimize_event *event = data;
    vela_view_set_minimized(view, event->minimize);
}

static void handle_request_activate(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_activate);
    vela_focus_new_view(view->server, view);
}

static void handle_set_title(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, set_title);
    vela_view_title_changed(view);
    vela_view_refresh_decoration(view);
}

static void handle_set_decorations(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, set_decorations);
    if (view->mapped) {
        vela_view_update_decoration(view);
    }
}

static void handle_set_class(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, set_app_id);
    vela_view_app_id_changed(view);
}

static void handle_set_parent(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, set_parent);
    vela_view_parent_changed(view);
}

static void handle_request_below(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_below);
    // wlroots has already updated the flag. Wine asks after mapping, when the
    // backdrop may already have the keyboard and cover the game: give both back.
    vela_view_keep_below(view);
    if (view->mapped && vela_view_stays_below(view)) {
        // Alt+Tab caches its candidates when opened; discard a window that
        // became a backdrop during an active switcher session.
        vela_switcher_forget(view->server, view);
    }
    if (view->mapped && vela_view_stays_below(view) && vela_views_focused(view->server) == view) {
        vela_focus_refocus(view->server);
    }
}

static void handle_request_skip_taskbar(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_skip_taskbar);
    // wlroots has already updated the flag: show or hide the taskbar entry.
    vela_view_show_in_taskbar(view, !view->x11->skip_taskbar && vela_view_on_current_workspace(view));
}

static void handle_set_override_redirect(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, set_override_redirect);
    struct vela_server *server = view->server;
    struct wlr_xwayland_surface *x11 = view->x11;
    // Do not demote an ordinary managed view on a redundant X11 notification.
    if (!x11->override_redirect) {
        return;
    }
    bool mapped = view->mapped;
    cancel_wine_style(view);
    cancel_initial_state(view);
    if (mapped) {
        // The underlying Wayland surface is still live. Closing snapshots
        // would leave a fading ghost over its replacement unmanaged surface.
        view->close_animated = true;
        vela_view_unmapped(view);
    }
    x11->data = NULL;
    vela_view_destroy(view);
    struct unmanaged *menu = unmanaged_create(server, x11);
    if (x11->surface) {
        handle_unmanaged_associate(&menu->associate, NULL);
        if (mapped) {
            handle_unmanaged_map(&menu->map, NULL);
        }
    }
}

static struct vela_view *view_create(struct vela_server *server, struct wlr_xwayland_surface *x11)
{
    struct vela_view *view = calloc(1, sizeof(*view));
    vela_view_init(view, server);
    view->x11 = x11;
    view->x11_density = 1.0;
    x11->data = view; // to get back to the parent window
    vela_listen(&x11->events.associate, &view->associate, handle_associate);
    vela_listen(&x11->events.dissociate, &view->dissociate, handle_dissociate);
    vela_listen(&x11->events.destroy, &view->destroy, handle_destroy);
    vela_listen(&x11->events.map_request, &view->map_request, handle_map_request);
    vela_listen(&x11->events.request_configure, &view->request_configure, handle_request_configure);
    vela_listen(&x11->events.request_move, &view->request_move, handle_request_move);
    vela_listen(&x11->events.request_resize, &view->request_resize, handle_request_resize);
    vela_listen(&x11->events.request_maximize, &view->request_maximize, handle_request_maximize);
    vela_listen(&x11->events.request_fullscreen, &view->request_fullscreen, handle_request_fullscreen);
    vela_listen(&x11->events.request_minimize, &view->request_minimize, handle_request_minimize);
    vela_listen(&x11->events.request_activate, &view->request_activate, handle_request_activate);
    vela_listen(&x11->events.set_title, &view->set_title, handle_set_title);
    vela_listen(&x11->events.set_decorations, &view->set_decorations, handle_set_decorations);
    vela_listen(&x11->events.set_class, &view->set_app_id, handle_set_class);
    vela_listen(&x11->events.set_parent, &view->set_parent, handle_set_parent);
    vela_listen(&x11->events.request_below, &view->request_below, handle_request_below);
    vela_listen(&x11->events.request_skip_taskbar, &view->request_skip_taskbar, handle_request_skip_taskbar);
    vela_listen(&x11->events.set_override_redirect, &view->set_override_redirect, handle_set_override_redirect);
    return view;
}

// --------------------------------------------------------------- Xwayland --

// wlroots' X window manager can leave events stuck in xcb's queue: when it
// flushes requests outside its event handler, xcb also reads whatever has
// arrived, and the queue is drained only when the socket becomes readable
// again. A quiet client then waits forever: the one that starts Xwayland
// creates its window while the window manager is being set up, and any
// window can lose the message that pairs it with its Wayland surface.
// Touching a root property makes the server send the window manager an
// event, which drains the queue.
static void wake_window_manager(struct vela_server *server)
{
    xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(server->xwayland);
    if (!connection || !server->xwayland_wake_atom) {
        return;
    }
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, screen->root, server->xwayland_wake_atom,
        XCB_ATOM_CARDINAL, 32, 0, NULL);
    xcb_flush(connection);
}

// X11 apps scale themselves (§3.9): Xft.dpi in the root's resources, read
// when they start. Appended, not replaced: what xrdb added stays, and the
// last value wins. Written at the window manager's start, before Xwayland
// accepts the other clients, when the largest scale changes and when someone
// replaces the resources without Xft.dpi (request_resources).
static void publish_dpi(struct vela_server *server)
{
    xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(server->xwayland);
    int dpi = vela_x11_root_dpi(server->x11_root);
    if (!connection || xcb_connection_has_error(connection) || dpi == server->xwayland_dpi) {
        return;
    }
    char line[32];
    // Starting a line of its own even if the last one has no newline: empty
    // lines are ignored.
    int length = snprintf(line, sizeof(line), "\nXft.dpi:\t%d\n", dpi);
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    xcb_change_property(connection, XCB_PROP_MODE_APPEND, screen->root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING,
        8, (uint32_t)length, line);
    xcb_flush(connection);
    server->xwayland_dpi = dpi;
    wlr_log(WLR_INFO, "Xwayland: Xft.dpi %d", dpi);
}

// Xwayland has given an X window its Wayland surface: the pairing message
// on the X side must not be left in the queue.
static void handle_shell_surface(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, xwayland_shell_surface);
    wake_window_manager(server);
}

static void handle_ready(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, xwayland_ready);
    // A new X server (the first, or one restarted): empty resources.
    server->xwayland_dpi = 0;
    server->xwayland_resources_pending = false;
    publish_dpi(server);
    wlr_xwayland_set_seat(server->xwayland, server->seat);
    // The cursor for X11 windows until the app picks one.
    if (wlr_xcursor_manager_load(server->cursor_manager, 1.0f)) {
        struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(server->cursor_manager, "default", 1.0f);
        if (xcursor) {
            struct wlr_xcursor_image *image = xcursor->images[0];
            wlr_xwayland_set_cursor(server->xwayland, wlr_xcursor_image_get_buffer(image), (int32_t)image->hotspot_x,
                (int32_t)image->hotspot_y);
        }
    }
    // The atom is asked for once, here: later a reply could keep us waiting
    // on an Xwayland that is itself waiting on us.
    xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(server->xwayland);
    static const char name[] = "_VELA_WAKE";
    xcb_intern_atom_reply_t *atom
        = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, sizeof(name) - 1, name), NULL);
    server->xwayland_wake_atom = atom ? atom->atom : XCB_ATOM_NONE;
    free(atom);
    static const char wine_style_name[] = "_WINE_HWND_STYLE";
    atom = xcb_intern_atom_reply(connection,
        xcb_intern_atom(connection, 0, sizeof(wine_style_name) - 1, wine_style_name), NULL);
    server->xwayland_wine_style_atom = atom ? atom->atom : XCB_ATOM_NONE;
    free(atom);
    static const char *const state_names[] = { "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN",
        "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ" };
    xcb_intern_atom_cookie_t cookies[4];
    for (int i = 0; i < 4; i++) {
        cookies[i] = xcb_intern_atom(connection, 0, (uint16_t)strlen(state_names[i]), state_names[i]);
    }
    for (int i = 0; i < 4; i++) {
        atom = xcb_intern_atom_reply(connection, cookies[i], NULL);
        server->xwayland_state_atoms[i] = atom ? atom->atom : XCB_ATOM_NONE;
        free(atom);
    }
    wake_window_manager(server);
    wlr_log(WLR_INFO, "Xwayland ready on DISPLAY=%s", server->xwayland->display_name);
}

static void handle_new_surface(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, xwayland_new_surface);
    struct wlr_xwayland_surface *surface = data;
    if (surface->override_redirect) {
        unmanaged_create(server, surface);
    } else {
        view_create(server, surface);
    }
}

void vela_xwayland_init(struct vela_server *server)
{
    wl_list_init(&server->xwayland_ready.link);
    wl_list_init(&server->xwayland_new_surface.link);
    wl_list_init(&server->xwayland_shell_surface.link);
    // Lazy: the X server starts with the first client, no cost when unused.
    server->xwayland = wlr_xwayland_create(server->display, server->compositor, true);
    if (!server->xwayland) {
        wlr_log(WLR_ERROR, "Xwayland not available: X11-only apps won't start");
        return;
    }
    // The outputs in pixels, for Xwayland alone.
    server->x11_root = vela_x11_root_create(server);
    if (!server->x11_root) {
        wlr_log(WLR_ERROR, "Xwayland: no xdg-output of its own, X11 apps get logical sizes");
    }
    // Observe Wine's _WINE_HWND_STYLE notifications while forwarding every
    // event to wlroots for normal XWM management.
    server->xwayland->data = server;
    server->xwayland_property_timer = wl_event_loop_add_timer(server->loop, poll_property_replies, server);
    if (server->xwayland_property_timer) {
        server->xwayland->user_event_handler = handle_x11_event;
    } else {
        wlr_log(WLR_ERROR, "Xwayland: cannot read initial states and Wine style changes (timer creation failed)");
    }
    vela_listen(&server->xwayland->events.ready, &server->xwayland_ready, handle_ready);
    vela_listen(&server->xwayland->events.new_surface, &server->xwayland_new_surface, handle_new_surface);
    vela_listen(&server->xwayland->shell_v1->events.new_surface, &server->xwayland_shell_surface, handle_shell_surface);
    wlr_log(WLR_INFO, "Xwayland on DISPLAY=%s (starts with the first X11 client)", server->xwayland->display_name);
}

void vela_xwayland_finish(struct vela_server *server)
{
    if (server->xwayland_property_timer) {
        wl_event_source_remove(server->xwayland_property_timer);
        server->xwayland_property_timer = NULL;
    }
    // Closes the X11 windows (and their views) before the Wayland clients.
    if (server->xwayland) {
        vela_unlisten(&server->xwayland_ready);
        vela_unlisten(&server->xwayland_new_surface);
        vela_unlisten(&server->xwayland_shell_surface);
        wlr_xwayland_destroy(server->xwayland);
        server->xwayland = NULL;
    }
    vela_x11_root_destroy(server->x11_root);
    server->x11_root = NULL;
}

void vela_xwayland_sync(struct vela_server *server)
{
    if (!server->xwayland) {
        return;
    }
    publish_dpi(server);
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->x11) {
            vela_view_sync_x11(view);
        }
    }
}

#else

void vela_view_sync_x11(struct vela_view *view)
{
}

void vela_xwayland_init(struct vela_server *server)
{
}

void vela_xwayland_finish(struct vela_server *server)
{
}

void vela_xwayland_sync(struct vela_server *server)
{
}

#endif
