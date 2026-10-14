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
// X11 apps don't know fractional scaling: they draw at 1× and the compositor
// magnifies them with the quality filter (docs/renderer.md §3.9).

#include "view.h"

#include "interact.h"
#include "focus.h"
#include "bindings.h"
#include "input.h"
#include "listen.h"
#include "output.h"
#include "scene/scene.h"
#include "server.h"
#include "snap.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/config.h>
#include <wlr/util/log.h>

#if WLR_HAS_XWAYLAND

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xcursor.h>
#include <wlr/xwayland.h>
#include <wlr/xwayland/shell.h>

// Runtime-only, opt-in diagnostics: no logging cost or retained history unless
// explicitly enabled through the private Vela command socket. Track IDs and
// geometry, not potentially private application titles or buffer content.
static void trace_x11(struct vela_view *view, const char *event, const char *format, ...)
{
    struct vela_server *server = view->server;
    if (!server->trace_x11_enabled) {
        return;
    }
    struct wlr_xwayland_surface *x11 = view->x11;
    char *entry = server->trace_x11_events[server->trace_x11_total % 128];
    int n = snprintf(entry, 192, "xid=0x%08x event=%s x11fullscreen=%d velafullscreen=%d mapped=%d "
        "x11geo=%d,%d:%ux%u ", (unsigned)x11->window_id, event, x11->fullscreen, view->fullscreen, view->mapped,
        x11->x, x11->y, (unsigned)x11->width, (unsigned)x11->height);
    if (n < 0) {
        return;
    }
    if (n < 192 && format) {
        va_list args;
        va_start(args, format);
        vsnprintf(entry + n, (size_t)(192 - n), format, args);
        va_end(args);
    }
    ++server->trace_x11_total;
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
    struct wl_listener destroy;
};

static void handle_unmanaged_map(struct wl_listener *listener, void *data)
{
    struct unmanaged *menu = wl_container_of(listener, menu, map);
    vela_node_set_position(&menu->tree->node, menu->x11->x, menu->x11->y);
    vela_node_raise_to_top(&menu->tree->node);
    vela_node_set_enabled(&menu->tree->node, true);
    // Some X11 menus want the keyboard (to move through the entries).
    if (wlr_xwayland_surface_override_redirect_wants_focus(menu->x11)) {
        vela_input_keyboard_enter(menu->server->input, menu->x11->surface);
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
    vela_node_set_position(&menu->tree->node, menu->x11->x, menu->x11->y);
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
    vela_unlisten(&menu->destroy);
    if (menu->surface_node) {
        vela_node_destroy(&menu->surface_node->node);
    }
    vela_node_destroy(&menu->tree->node);
    free(menu);
}

static void unmanaged_create(struct vela_server *server, struct wlr_xwayland_surface *x11)
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
    vela_listen(&x11->events.destroy, &menu->destroy, handle_unmanaged_destroy);
}

// ----------------------------------------------------------- X11 windows --

void vela_view_sync_x11(struct vela_view *view)
{
    struct wlr_xwayland_surface *x11 = view->x11;
    if (!x11 || !x11->surface) {
        return;
    }
    const struct wlr_box wanted = {
        .x = (int)lround(view->tree->node.x),
        .y = (int)lround(view->tree->node.y),
        .width = view->x11_width > 0 ? view->x11_width : x11->width,
        .height = view->x11_height > 0 ? view->x11_height : x11->height,
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

static void handle_map(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, map);
    trace_x11(view, "map", "");
    vela_view_update_decoration(view);
    // X11 apps can ask to start maximized or fullscreen (games).
    if (view->x11->fullscreen) {
        vela_view_set_fullscreen(view, true);
    } else if (view->x11->maximized_horz || view->x11->maximized_vert) {
        vela_view_set_maximized(view, true, true);
    }
    vela_view_mapped(view);
    trace_x11(view, "mapped", "");
}

static void handle_unmap(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, unmap);
    trace_x11(view, "unmap", "");
    vela_view_unmapped(view);
}

static void handle_commit(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, commit);
    if (view->server->trace_x11_enabled && view->x11->surface) {
        int width = view->x11->surface->current.width;
        int height = view->x11->surface->current.height;
        if (width != view->x11_trace_width || height != view->x11_trace_height) {
            trace_x11(view, "buffer-size", "surface=%dx%d", width, height);
            view->x11_trace_width = width;
            view->x11_trace_height = height;
        }
    }
    vela_view_committed(view);
}

// The Wayland surface comes later (associate) and can leave before the X11
// window (dissociate).
static void handle_associate(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, associate);
    trace_x11(view, "associate", "");
    struct wlr_surface *surface = view->x11->surface;
    view->surface_node = vela_surface_node_create(view->tree, surface);
    vela_listen(&surface->events.map, &view->map, handle_map);
    vela_listen(&surface->events.unmap, &view->unmap, handle_unmap);
    vela_listen(&surface->events.commit, &view->commit, handle_commit);
    map_if_drawn(surface);
}

static void handle_dissociate(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, dissociate);
    trace_x11(view, "dissociate", "");
    view->x11_trace_width = 0;
    view->x11_trace_height = 0;
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
    trace_x11(view, "destroy", "");
    vela_view_destroy(view);
}

static void handle_request_configure(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    trace_x11(view, "request-configure", "request=%d,%d:%ux%u mask=0x%x policy=%s",
        event->x, event->y, (unsigned)event->width, (unsigned)event->height, (unsigned)event->mask,
        !view->mapped ? "initial" : (view->maximized || view->fullscreen || !vela_snap_is_none(view->snap))
            ? "constrained" : "free");
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
    // size it remembers, even when they no longer fit.
    int bar = vela_view_title_bar_height(view);
    struct wlr_box frame = { event->x, event->y - bar, event->width, event->height + bar };
    struct vela_output *output
        = vela_output_at(view->server, frame.x + frame.width / 2.0, frame.y + frame.height / 2.0);
    if (!output) {
        output = vela_view_output(view);
    }
    if (output) {
        frame = vela_output_fit(output, frame);
    }
    vela_node_set_position(&view->tree->node, frame.x, frame.y + bar);
    view->x11_width = frame.width;
    view->x11_height = frame.height - bar;
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
    trace_x11(view, "request-fullscreen", "requested=%d", view->x11->fullscreen);
    vela_view_set_fullscreen(view, view->x11->fullscreen);
    trace_x11(view, "fullscreen-applied", "requested=%d", view->x11->fullscreen);
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

static void view_create(struct vela_server *server, struct wlr_xwayland_surface *x11)
{
    struct vela_view *view = calloc(1, sizeof(*view));
    vela_view_init(view, server);
    view->x11 = x11;
    x11->data = view; // to get back to the parent window
    vela_listen(&x11->events.associate, &view->associate, handle_associate);
    vela_listen(&x11->events.dissociate, &view->dissociate, handle_dissociate);
    vela_listen(&x11->events.destroy, &view->destroy, handle_destroy);
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
    vela_listen(&server->xwayland->events.ready, &server->xwayland_ready, handle_ready);
    vela_listen(&server->xwayland->events.new_surface, &server->xwayland_new_surface, handle_new_surface);
    vela_listen(&server->xwayland->shell_v1->events.new_surface, &server->xwayland_shell_surface, handle_shell_surface);
    wlr_log(WLR_INFO, "Xwayland on DISPLAY=%s (starts with the first X11 client)", server->xwayland->display_name);
}

void vela_xwayland_finish(struct vela_server *server)
{
    // Closes the X11 windows (and their views) before the Wayland clients.
    if (server->xwayland) {
        vela_unlisten(&server->xwayland_ready);
        vela_unlisten(&server->xwayland_new_surface);
        vela_unlisten(&server->xwayland_shell_surface);
        wlr_xwayland_destroy(server->xwayland);
        server->xwayland = NULL;
    }
}

void vela_xwayland_sync(struct vela_server *server)
{
    if (!server->xwayland) {
        return;
    }
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
