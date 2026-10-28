// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "view.h"

#include "interact.h"
#include "focus.h"
#include "bindings.h"
#include "decoration.h"
#include "layer.h"
#include "listen.h"
#include "output.h"
#include "popup.h"
#include "scene/capture.h"
#include "scene/scene.h"
#include "server.h"
#include "snapshot.h"
#include "util.h"
#include "workspace.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/config.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb_icccm.h>
#endif

#define CORNER_RADIUS 8.0 // logical, like Windows 11

// ------------------------------------------------------------ to the app --

struct wlr_surface *vela_view_surface(const struct vela_view *view)
{
#if WLR_HAS_XWAYLAND
    if (view->x11) {
        return view->x11->surface;
    }
#endif
    return view->xdg->base->surface;
}

int vela_view_title_bar_height(const struct vela_view *view)
{
    return view->decoration && !view->fullscreen ? VELA_DECORATION_HEIGHT : 0;
}

struct wlr_box vela_view_geometry(const struct vela_view *view)
{
    int bar = vela_view_title_bar_height(view);
    if (view->xdg) {
        // With Vela's bar the frame starts above the surface.
        struct wlr_box g = view->xdg->base->geometry;
        return (struct wlr_box) { g.x, g.y - bar, g.width, g.height + bar };
    }
#if WLR_HAS_XWAYLAND
    // X11: no shadow margins; Vela's bar, if any, sits on top.
    return (struct wlr_box) { 0, -bar, view->x11->width, view->x11->height + bar };
#else
    return (struct wlr_box) { 0 };
#endif
}

bool vela_view_configurable(const struct vela_view *view)
{
    if (view->xdg) {
        return view->xdg->base->initialized;
    }
#if WLR_HAS_XWAYLAND
    return view->x11->surface != NULL;
#else
    return false;
#endif
}

const char *vela_view_title(const struct vela_view *view)
{
    const char *title = view->xdg ? view->xdg->title : NULL;
#if WLR_HAS_XWAYLAND
    if (view->x11) {
        title = view->x11->title;
    }
#endif
    return title ? title : "";
}

const char *vela_view_app_id(const struct vela_view *view)
{
    // X11: the window class (WM_CLASS), which serves as app id.
    const char *id = view->xdg ? view->xdg->app_id : NULL;
#if WLR_HAS_XWAYLAND
    if (view->x11) {
        id = view->x11->class;
    }
#endif
    return id ? id : "";
}

bool vela_view_is_system_prompt(const struct vela_view *view)
{
    // By app id (Wayland) or class (X11): KWallet and ksecretd, KDE's polkit
    // agent, pinentry, GNOME's keyring prompt.
    const char *app = vela_view_app_id(view);
    char id[256];
    size_t n = 0;
    for (; app[n] && n + 1 < sizeof(id); ++n) {
        id[n] = (char)tolower((unsigned char)app[n]);
    }
    id[n] = '\0';
    const char *known[] = {
        "kwalletd", "ksecretd", "polkit-kde-authentication-agent", "pinentry", "gcr-prompter", "systemprompter",
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (strstr(id, known[i])) {
            return true;
        }
    }
    return false;
}

static struct vela_view *view_parent(const struct vela_view *view)
{
    if (view->xdg) {
        return view->xdg->parent ? view->xdg->parent->base->data : NULL;
    }
#if WLR_HAS_XWAYLAND
    return view->x11->parent ? view->x11->parent->data : NULL;
#else
    return NULL;
#endif
}

void vela_view_configure_size(struct vela_view *view, int width, int height)
{
    // Sizes are of the whole frame: the app gets the part below Vela's bar.
    int app_height = height > 0 ? vela_max(1, height - vela_view_title_bar_height(view)) : 0;
    if (view->xdg) {
        wlr_xdg_toplevel_set_size(view->xdg, width, app_height);
        return;
    }
    view->x11_width = width;
    view->x11_height = app_height;
    vela_view_sync_x11(view);
}

void vela_view_send_maximized(struct vela_view *view, bool on)
{
    if (view->xdg) {
        wlr_xdg_toplevel_set_maximized(view->xdg, on);
        return;
    }
#if WLR_HAS_XWAYLAND
    wlr_xwayland_surface_set_maximized(view->x11, on, on);
#endif
}

static void send_fullscreen(struct vela_view *view, bool on)
{
    if (view->xdg) {
        wlr_xdg_toplevel_set_fullscreen(view->xdg, on);
        return;
    }
#if WLR_HAS_XWAYLAND
    wlr_xwayland_surface_set_fullscreen(view->x11, on);
#endif
}

void vela_view_send_tiled(struct vela_view *view, uint32_t edges)
{
    if (view->xdg) {
        wlr_xdg_toplevel_set_tiled(view->xdg, edges);
    }
}

static void send_activated(struct vela_view *view, bool on)
{
    if (view->xdg) {
        if (view->xdg->base->initialized) {
            wlr_xdg_toplevel_set_activated(view->xdg, on);
        }
        return;
    }
#if WLR_HAS_XWAYLAND
    if (view->x11->surface) {
        wlr_xwayland_surface_activate(view->x11, on);
        if (on) {
            wlr_xwayland_surface_restack(view->x11, NULL, XCB_STACK_MODE_ABOVE);
        }
    }
#endif
}

void vela_view_close(struct vela_view *view)
{
    if (view->xdg) {
        wlr_xdg_toplevel_send_close(view->xdg);
        return;
    }
#if WLR_HAS_XWAYLAND
    wlr_xwayland_surface_close(view->x11);
#endif
}

bool vela_view_resizable(const struct vela_view *view)
{
    if (view->xdg) {
        const struct wlr_xdg_toplevel_state *state = &view->xdg->current;
        bool fixed_width = state->max_width > 0 && state->min_width == state->max_width;
        bool fixed_height = state->max_height > 0 && state->min_height == state->max_height;
        return !(fixed_width && fixed_height);
    }
#if WLR_HAS_XWAYLAND
    if (view->x11 && view->x11->size_hints) {
        const xcb_size_hints_t *hints = view->x11->size_hints;
        bool has_min = hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE;
        bool has_max = hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
        return !(has_min && has_max && hints->min_width == hints->max_width
            && hints->min_height == hints->max_height);
    }
#endif
    return true;
}

pid_t vela_view_pid(const struct vela_view *view)
{
#if WLR_HAS_XWAYLAND
    if (view->x11) {
        return view->x11->pid;
    }
#endif
    if (view->xdg && view->xdg->resource) {
        pid_t pid = 0;
        wl_client_get_credentials(wl_resource_get_client(view->xdg->resource), &pid, NULL, NULL);
        return pid;
    }
    return 0;
}

// -------------------------------------------------------------- geometry --

struct wlr_box vela_view_frame_box(struct vela_view *view)
{
    struct wlr_box geometry = vela_view_geometry(view);
    return (struct wlr_box) {
        (int)lround(view->tree->node.x) + geometry.x,
        (int)lround(view->tree->node.y) + geometry.y,
        geometry.width,
        geometry.height,
    };
}

// Resolve a mapped XDG parent by protocol identity, not a retained view
// pointer. This also works before the child has entered server->views.
static struct vela_view *mapped_xdg_parent(struct vela_view *view)
{
    if (!view->xdg || !view->xdg->parent) {
        return NULL;
    }
    struct vela_view *candidate;
    wl_list_for_each (candidate, &view->server->views, link) {
        if (candidate->mapped && candidate->xdg == view->xdg->parent) {
            return candidate;
        }
    }
    return NULL;
}

static struct vela_output *initial_output(struct vela_view *view)
{
    struct vela_view *parent = mapped_xdg_parent(view);
    if (!parent) {
        return vela_output_under_cursor(view->server);
    }
    // Layout changes defer window rescue to an idle callback. A transient
    // can map first, so repair its parent synchronously.
    struct vela_server *server = view->server;
    if (server->output_check) {
        wl_event_source_remove(server->output_check);
        server->output_check = NULL;
        vela_views_check_outputs(server);
    }
    return vela_view_output(parent);
}

struct vela_output *vela_view_output(struct vela_view *view)
{
    struct wlr_box frame = vela_view_frame_box(view);
    struct vela_output *output = vela_output_at(view->server, frame.x + frame.width / 2.0, frame.y + frame.height / 2.0);
    return output ? output : vela_output_under_cursor(view->server);
}

// The taskbar showing a window's button, if it still exists.
static struct vela_layer_surface *taskbar_layer(struct vela_server *server, struct wlr_surface *surface)
{
    struct vela_layer_surface *layer;
    wl_list_for_each (layer, &server->layer_surfaces, link) {
        if (layer->wlr->surface == surface) {
            return layer;
        }
    }
    return NULL;
}

struct wlr_box vela_view_minimize_target(struct vela_view *view)
{
    // Its button on the taskbar of its own output: every output has a
    // taskbar, and a window flying to another output's would leave the screen
    // at once.
    struct vela_output *output = vela_view_output(view);
    for (int i = 0; output && i < VELA_TASKBAR_BUTTONS; ++i) {
        struct vela_layer_surface *layer = view->taskbar_buttons[i].surface
            ? taskbar_layer(view->server, view->taskbar_buttons[i].surface)
            : NULL;
        if (layer && layer->mapped && layer->wlr->output == output->wlr) {
            double lx = 0.0;
            double ly = 0.0;
            vela_node_coords(&layer->tree->node, &lx, &ly);
            struct wlr_box box = view->taskbar_buttons[i].box;
            box.x += (int)lround(lx);
            box.y += (int)lround(ly);
            return box;
        }
    }
    // Unknown button: the center of the output's bottom edge.
    struct wlr_box area = output ? vela_output_box(output) : (struct wlr_box) { 0, 0, 1920, 1080 };
    return (struct wlr_box) { area.x + area.width / 2 - 24, area.y + area.height - 48, 48, 48 };
}

// Where the window returns when it stops being maximized or fullscreen. If it
// appeared that way (many apps remember their state), there is no saved
// position: the size the X11 app asked for is used, otherwise two thirds of
// the output, centered.
struct wlr_box vela_view_restore_box(struct vela_view *view)
{
    if (view->restore.width > 0 && view->restore.height > 0) {
        return view->restore;
    }
    struct vela_output *output = vela_view_output(view);
    if (!output) {
        return view->restore;
    }
    const struct wlr_box area = output->usable;
    struct wlr_box frame = { 0, 0, area.width * 2 / 3, area.height * 2 / 3 };
    // A game opened directly in fullscreen may report its monitor-sized
    // geometry as the "initial" X11 size. Restoring that would recreate the
    // same oversized window instead of making a usable normal window.
    struct wlr_box full = vela_output_box(output);
    bool initial_fullscreen = view->x11 && view->x11_initial.width >= full.width
        && view->x11_initial.height >= full.height;
    if (view->x11 && !initial_fullscreen && view->x11_initial.width > 0
        && view->x11_initial.height > 0) {
        frame.width = view->x11_initial.width;
        frame.height = view->x11_initial.height + (view->decoration ? VELA_DECORATION_HEIGHT : 0);
    }
    frame = vela_output_fit(output, frame);
    frame.x = area.x + (area.width - frame.width) / 2;
    frame.y = area.y + (area.height - frame.height) / 2;
    return frame;
}

void vela_view_keep_in_place(struct vela_view *view)
{
    // Maximized or fullscreen windows can change their shadow margin
    // (geometry.x/y) when redrawing: we keep them aligned.
    if (view->animating || (!view->maximized && !view->fullscreen && vela_snap_is_none(view->snap))) {
        return;
    }
    struct vela_output *output = vela_view_output(view);
    if (!output) {
        return;
    }
    struct vela_area area = view->fullscreen ? vela_output_full_area(output)
        : view->maximized                    ? vela_output_usable_area(output)
                                             : vela_snap_area(output, view->snap);
    struct vela_placement place = vela_output_place(output, area);
    struct wlr_box geometry = vela_view_geometry(view);
    vela_node_set_position(&view->tree->node, place.x - geometry.x, place.y - geometry.y);
}

// --------------------------------------------------------------- Vela's bar --

static struct vela_decoration_state decoration_state(struct vela_view *view)
{
    struct vela_output *output = vela_view_output(view);
    return (struct vela_decoration_state) {
        .geometry = vela_view_geometry(view),
        .scale = output ? output->wlr->scale : 1.0f,
        .title = vela_view_title(view),
        .app_id = vela_view_app_id(view),
        .active = view->activated,
        .maximized = view->maximized,
        .fullscreen = view->fullscreen,
    };
}

void vela_view_refresh_decoration(struct vela_view *view)
{
    if (view->decoration) {
        struct vela_decoration_state state = decoration_state(view);
        vela_decoration_update(view->decoration, &state);
    }
}

void vela_view_update_decoration(struct vela_view *view)
{
    // X11 windows that leave the bar to the window manager (apps with a bar of
    // their own, like Steam, say no), but not splash screens; Wayland apps
    // that accept a server-side bar (xdg-decoration: Qt and KDE, Chromium if
    // chosen).
    bool wanted = view->xdg_decoration
        && view->xdg_decoration->current.mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
#if WLR_HAS_XWAYLAND
    if (view->x11) {
        wanted = !view->x11->override_redirect && view->x11->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL
            && !wlr_xwayland_surface_has_window_type(view->x11, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH);
    }
#endif
    if (wanted && !view->decoration) {
        struct vela_decoration_state state = decoration_state(view); // still without the bar in the geometry
        view->decoration = vela_decoration_create(view->server, view->tree, &state);
    } else if (!wanted && view->decoration) {
        vela_decoration_destroy(view->decoration);
        view->decoration = NULL;
    } else {
        vela_view_refresh_decoration(view);
    }
}

// Vela's bar, unless the app explicitly asks to draw its own (§9.1): Chromium
// and Firefox with tabs in place of the title would draw theirs anyway, and
// the buttons would be doubled. Those that ask for nothing, or for ours, get
// Vela's.
static enum wlr_xdg_toplevel_decoration_v1_mode decoration_mode(const struct wlr_xdg_toplevel_decoration_v1 *decoration)
{
    return decoration->requested_mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
        ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
        : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

static void handle_decoration_mode(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, decoration_mode);
    if (view->xdg->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(view->xdg_decoration, decoration_mode(view->xdg_decoration));
    }
}

static void handle_decoration_destroy(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, decoration_destroy);
    vela_unlisten(&view->decoration_mode);
    vela_unlisten(&view->decoration_destroy);
    view->xdg_decoration = NULL;
    vela_view_update_decoration(view);
}

static void set_xdg_decoration(struct vela_view *view, struct wlr_xdg_toplevel_decoration_v1 *decoration)
{
    view->xdg_decoration = decoration;
    vela_unlisten(&view->decoration_mode);
    vela_unlisten(&view->decoration_destroy);
    vela_listen(&decoration->events.request_mode, &view->decoration_mode, handle_decoration_mode);
    vela_listen(&decoration->events.destroy, &view->decoration_destroy, handle_decoration_destroy);
    if (view->xdg->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration, decoration_mode(decoration));
    }
}

static void handle_new_decoration(struct wl_listener *listener, void *data)
{
    struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
    struct vela_view *view = decoration->toplevel->base->data;
    if (view) {
        set_xdg_decoration(view, decoration);
    }
}

// ---------------------------------------------------------------- taskbar --

static void update_ext_handle(struct vela_view *view)
{
    if (!view->ext_handle) {
        return;
    }
    const struct wlr_ext_foreign_toplevel_handle_v1_state state = {
        .title = vela_view_title(view),
        .app_id = vela_view_app_id(view),
    };
    wlr_ext_foreign_toplevel_handle_v1_update_state(view->ext_handle, &state);
}

// Dialogs declare the window they depend on, so the taskbar knows not to show
// them as separate apps.
static void update_handle_parent(struct vela_view *view)
{
    if (!view->handle) {
        return;
    }
    struct vela_view *owner = view_parent(view);
    wlr_foreign_toplevel_handle_v1_set_parent(view->handle, owner ? owner->handle : NULL);
}

void vela_view_title_changed(struct vela_view *view)
{
    if (view->handle) {
        wlr_foreign_toplevel_handle_v1_set_title(view->handle, vela_view_title(view));
    }
    update_ext_handle(view);
}

void vela_view_app_id_changed(struct vela_view *view)
{
    if (view->handle) {
        wlr_foreign_toplevel_handle_v1_set_app_id(view->handle, vela_view_app_id(view));
    }
    update_ext_handle(view);
}

// Keep a transient above its fullscreen ancestor, but do not globally
// elevate unrelated windows above fullscreen content.
static bool transient_has_fullscreen_ancestor(struct vela_view *view)
{
    struct vela_view *parent = view_parent(view);
    for (int depth = 0; parent && depth < 64; ++depth) {
        if (!parent->mapped) {
            return false;
        }
        if (parent->fullscreen) {
            return true;
        }
        parent = view_parent(parent);
    }
    return false;
}

static void update_transient_layer(struct vela_view *view)
{
    if (!view->mapped || view->fullscreen) {
        return;
    }
    // A formerly fullscreen child must return to the normal window layer
    // when its parent is destroyed or its XDG parent relationship is cleared.
    if (!view_parent(view) && view->tree->node.parent != view->server->layers.fullscreen) {
        return;
    }
    struct vela_tree *target = transient_has_fullscreen_ancestor(view)
        ? view->server->layers.fullscreen : view->server->layers.windows;
    if (view->tree->node.parent != target) {
        vela_node_reparent(&view->tree->node, target);
    }
}

static void update_transient_layers(struct vela_server *server)
{
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        update_transient_layer(view);
    }
}

// Focusing a fullscreen parent must not raise it over an existing dialog.
// Keep each mapped child above the immediate parent, recursively, including
// when a modal dialog has its own transient descendants.
static void raise_transient_descendants(struct vela_view *parent, unsigned depth)
{
    if (depth >= 32) {
        return;
    }
    struct vela_view *child;
    wl_list_for_each_reverse (child, &parent->server->views, link) {
        if (!child->mapped || child == parent || view_parent(child) != parent
            || child->tree->node.parent != parent->tree->node.parent) {
            continue;
        }
        vela_node_raise_to_top(&child->tree->node);
        raise_transient_descendants(child, depth + 1);
    }
}

void vela_views_raise_transients(struct vela_view *view)
{
    raise_transient_descendants(view, 0);
}

void vela_view_parent_changed(struct vela_view *view)
{
    update_handle_parent(view);
    // Descendants of this view can also need to leave/enter fullscreen layer.
    update_transient_layers(view->server);
}

static void handle_taskbar_activate(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, handle_activate);
    vela_focus_view(view->server, view);
}

static void handle_taskbar_close(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, handle_close);
    vela_view_close(view);
}

static void handle_taskbar_maximize(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, handle_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
    vela_view_set_maximized(view, event->maximized, true);
}

static void handle_taskbar_minimize(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, handle_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
    vela_view_set_minimized(view, event->minimized);
}

static void handle_taskbar_fullscreen(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, handle_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
    vela_view_set_fullscreen(view, event->fullscreen);
}

static void handle_taskbar_rectangle(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, handle_rectangle);
    struct wlr_foreign_toplevel_handle_v1_set_rectangle_event *event = data;
    // A taskbar tells us where this window's button is, in its surface
    // coordinates; each taskbar (one per output) its own. An empty one: no
    // button there.
    int slot = -1;
    for (int i = 0; i < VELA_TASKBAR_BUTTONS; ++i) {
        struct wlr_surface *surface = view->taskbar_buttons[i].surface;
        if (surface == event->surface) {
            slot = i;
            break;
        }
        // A free slot, or one of a taskbar that is gone.
        if (slot < 0 && (!surface || !taskbar_layer(view->server, surface))) {
            slot = i;
        }
    }
    if (slot < 0) {
        return;
    }
    bool shown = event->width > 0 && event->height > 0;
    view->taskbar_buttons[slot].surface = shown ? event->surface : NULL;
    view->taskbar_buttons[slot].box = (struct wlr_box) { event->x, event->y, event->width, event->height };
}

static void create_taskbar_handle(struct vela_view *view)
{
#if WLR_HAS_XWAYLAND
    if (view->x11 && view->x11->skip_taskbar) {
        return; // _NET_WM_STATE_SKIP_TASKBAR (Wine's fullscreen backdrop)
    }
#endif
    struct wlr_foreign_toplevel_handle_v1 *handle
        = wlr_foreign_toplevel_handle_v1_create(view->server->foreign_toplevels);
    view->handle = handle;
    handle->data = view;
    wlr_foreign_toplevel_handle_v1_set_title(handle, vela_view_title(view));
    wlr_foreign_toplevel_handle_v1_set_app_id(handle, vela_view_app_id(view));
    wlr_foreign_toplevel_handle_v1_set_maximized(handle, view->maximized);
    wlr_foreign_toplevel_handle_v1_set_fullscreen(handle, view->fullscreen);
    wlr_foreign_toplevel_handle_v1_set_minimized(handle, view->minimized);
    wlr_foreign_toplevel_handle_v1_set_activated(handle, view->activated);
    update_handle_parent(view);
    vela_listen(&handle->events.request_activate, &view->handle_activate, handle_taskbar_activate);
    vela_listen(&handle->events.request_close, &view->handle_close, handle_taskbar_close);
    vela_listen(&handle->events.request_maximize, &view->handle_maximize, handle_taskbar_maximize);
    vela_listen(&handle->events.request_minimize, &view->handle_minimize, handle_taskbar_minimize);
    vela_listen(&handle->events.request_fullscreen, &view->handle_fullscreen, handle_taskbar_fullscreen);
    vela_listen(&handle->events.set_rectangle, &view->handle_rectangle, handle_taskbar_rectangle);
}

static void destroy_taskbar_handle(struct vela_view *view)
{
    if (!view->handle) {
        return;
    }
    // wlroots checks that nobody listens to the handle any more when
    // destroying it.
    vela_unlisten(&view->handle_activate);
    vela_unlisten(&view->handle_close);
    vela_unlisten(&view->handle_maximize);
    vela_unlisten(&view->handle_minimize);
    vela_unlisten(&view->handle_fullscreen);
    vela_unlisten(&view->handle_rectangle);
    wlr_foreign_toplevel_handle_v1_destroy(view->handle);
    view->handle = NULL;
    memset(view->taskbar_buttons, 0, sizeof(view->taskbar_buttons));
}

static void create_handles(struct vela_view *view)
{
    const struct wlr_ext_foreign_toplevel_handle_v1_state state = {
        .title = vela_view_title(view),
        .app_id = vela_view_app_id(view),
    };
    view->ext_handle = wlr_ext_foreign_toplevel_handle_v1_create(view->server->ext_toplevels, &state);
    view->ext_handle->data = view;
    if (vela_view_on_current_workspace(view)) {
        create_taskbar_handle(view);
    }
}

static void destroy_handles(struct vela_view *view)
{
    if (view->ext_handle) {
        wlr_ext_foreign_toplevel_handle_v1_destroy(view->ext_handle);
        view->ext_handle = NULL;
    }
    destroy_taskbar_handle(view);
}

void vela_view_show_in_taskbar(struct vela_view *view, bool on)
{
    if (on && !view->handle && view->mapped) {
        create_taskbar_handle(view);
    } else if (!on && view->handle) {
        destroy_taskbar_handle(view);
    }
}

void vela_view_set_activated(struct vela_view *view, bool on)
{
    view->activated = on;
    send_activated(view, on);
    vela_view_refresh_decoration(view);
    if (view->handle) {
        wlr_foreign_toplevel_handle_v1_set_activated(view->handle, on);
    }
}

// ------------------------------------------------------------- animation --

static void apply_open_frame(struct vela_view *view, double progress)
{
    // Maximized and fullscreen windows only fade in, and so does every window
    // without animation effects.
    int rise = (view->maximized || view->fullscreen || !vela_motion_effects()) ? 0 : VELA_WINDOW_OPEN_RISE;
    double y = view->target_y + (1.0 - progress) * rise;
    vela_node_set_position(&view->tree->node, view->target_x, y);
    // Opacity reaches 1 a little before the motion ends: the window is
    // readable at once, the final motion is just "settling".
    float opacity = (float)fmin(1.0, progress * 1.4);
    vela_node_set_opacity(&view->tree->node, opacity);
    if (vela_motion_logger) {
        wlr_log(WLR_INFO, "animation: open #%u geometry %.3f %.3f 1 1 opacity %.3f", view->open_tween.serial,
            view->target_x, y, opacity);
    }
}

static void start_open_animation(struct vela_view *view)
{
    vela_tween_start(&view->open_tween, "open", vela_motion_ms(VELA_WINDOW_OPEN_MS), &vela_decelerate);
    view->animating = true;
    view->open_frames = 0;
    apply_open_frame(view, 0.0);
    vela_server_schedule_frames(view->server);
}

void vela_view_finish_open_animation(struct vela_view *view)
{
    if (!view->animating) {
        return;
    }
    view->animating = false;
    apply_open_frame(view, 1.0);
}

bool vela_views_tick(struct vela_server *server, double now_ms, double wall_ms)
{
    bool running = false;
    struct vela_view *view, *next;
    wl_list_for_each_safe (view, next, &server->views, link) {
        if (view->decoration) {
            running |= vela_decoration_tick(view->decoration, now_ms, wall_ms);
        }
        if (!view->animating) {
            continue;
        }
        apply_open_frame(view, vela_tween_progress(&view->open_tween, now_ms));
        ++view->open_frames;
        if (vela_tween_retired(&view->open_tween, wall_ms)) {
            wlr_log(WLR_DEBUG, "Open animation: %d frames", view->open_frames);
            vela_view_finish_open_animation(view);
        } else {
            running = true;
        }
    }
    return running;
}

// Apps with their own shadow margins (GTK) keep their own corners too.
static bool has_own_shadow(const struct vela_view *view)
{
    if (!view->xdg || !view->xdg->base->surface) {
        return false;
    }
    struct wlr_box g = view->xdg->base->geometry;
    int width = view->xdg->base->surface->current.width;
    int height = view->xdg->base->surface->current.height;
    return g.x > 0 || g.y > 0 || (g.width > 0 && g.width < width) || (g.height > 0 && g.height < height);
}

void vela_view_update_shape(struct vela_view *view)
{
    struct vela_shape shape = { 0 };
    if (view->mapped && !view->minimized && !view->maximized && !view->fullscreen && vela_snap_is_none(view->snap)
        && !has_own_shadow(view)) {
        struct wlr_box g = vela_view_geometry(view);
        if (g.width > 0 && g.height > 0) {
            shape.enabled = true;
            shape.x = g.x;
            shape.y = g.y;
            shape.width = g.width;
            shape.height = g.height;
            shape.radius = CORNER_RADIUS;
            shape.shadow = true;
            shape.active = view->activated;
        }
    }
    vela_tree_set_shape(view->tree, &shape);
}

// ----------------------------------------------------- maximize/fullscreen --

static void apply_maximized(struct vela_view *view)
{
    struct vela_output *output = view->mapped ? vela_view_output(view) : initial_output(view);
    if (!output || !vela_view_configurable(view)) {
        return;
    }
    vela_view_send_maximized(view, true);
    struct vela_placement place = vela_output_place(output, vela_output_usable_area(output));
    // Set the scene position first. vela_view_configure_size() sends an X11
    // ConfigureWindow request using the scene node's *current* coordinates.
    // Sending the new size first leaves Xwayland at the old window position.
    vela_view_keep_in_place(view);
    vela_view_configure_size(view, place.width, place.height);
    if (view->handle) {
        wlr_foreign_toplevel_handle_v1_set_maximized(view->handle, true);
    }
}

// Back to the free frame it had before being maximized or fullscreen.
static void restore(struct vela_view *view)
{
    struct wlr_box back = vela_view_restore_box(view);
    if (view->mapped && back.width > 0) {
        struct wlr_box geometry = vela_view_geometry(view);
        vela_node_set_position(&view->tree->node, back.x - geometry.x, back.y - geometry.y);
    }
    // For X11, configure after positioning: size-only then move-only
    // configurations can leave the X11 window offset from Vela's scene.
    vela_view_configure_size(view, back.width, back.height);
}

void vela_view_morph(struct vela_view *view, struct wlr_box to, bool corners)
{
    if (view->mapped && !view->minimized && view->tree->node.enabled && vela_view_on_current_workspace(view)) {
        vela_snapshot_morph(view->server, view, to, corners && !has_own_shadow(view) ? CORNER_RADIUS : 0.0);
    }
}

void vela_view_set_maximized(struct vela_view *view, bool on, bool animate)
{
    if (!vela_view_configurable(view)) {
        return;
    }
    if (on == view->maximized || view->fullscreen) {
        // The protocol wants an answer to the request anyway.
        if (view->xdg) {
            wlr_xdg_surface_schedule_configure(view->xdg->base);
        }
        return;
    }
    vela_view_finish_open_animation(view);

    if (on) {
        struct vela_output *output = vela_view_output(view);
        if (animate && output) {
            struct vela_placement place = vela_output_place(output, vela_output_usable_area(output));
            vela_view_morph(
                view, (struct wlr_box) { (int)lround(place.x), (int)lround(place.y), place.width, place.height }, false);
        }
        vela_view_leave_snap_group(view);
        if (view->mapped && vela_snap_is_none(view->snap)) {
            view->restore = vela_view_frame_box(view); // from snapped it goes back to the free size
        }
        if (!vela_snap_is_none(view->snap)) {
            view->snap = vela_snap_none;
            vela_view_send_tiled(view, WLR_EDGE_NONE);
        }
        view->maximized = true;
        apply_maximized(view);
        vela_view_refresh_decoration(view); // the button becomes "restore"
        return;
    }

    if (animate) {
        vela_view_morph(view, vela_view_restore_box(view), true);
    }
    view->maximized = false;
    vela_view_send_maximized(view, false);
    if (view->handle) {
        wlr_foreign_toplevel_handle_v1_set_maximized(view->handle, false);
    }
    restore(view);
    vela_view_refresh_decoration(view);
}

void vela_view_set_fullscreen(struct vela_view *view, bool on)
{
    if (!vela_view_configurable(view)) {
        return;
    }
    // Actual fullscreen state changes override an outstanding Wine restore
    // hint. Redundant requests must not: Wine can echo fullscreen-off after
    // Vela reconciled a popup -> decorated style change.
    if (on != view->fullscreen) {
        view->x11_wine_restore_fullscreen = false;
    }
    if (on == view->fullscreen) {
        if (view->xdg) {
            wlr_xdg_surface_schedule_configure(view->xdg->base);
        }
        return;
    }
    vela_view_finish_open_animation(view);
    struct vela_output *output = view->mapped ? vela_view_output(view) : initial_output(view);

    if (on) {
        if (view->mapped && !view->maximized && vela_snap_is_none(view->snap)) {
            view->restore = vela_view_frame_box(view);
        }
        view->fullscreen = true;
        send_fullscreen(view, true);
        if (view->handle) {
            wlr_foreign_toplevel_handle_v1_set_fullscreen(view->handle, true);
        }
        // Above taskbar and panels. Position the scene before configuring
        // the X11 window so it gets the final fullscreen origin (not the
        // old centered windowed origin) in the same ConfigureWindow request.
        vela_node_reparent(&view->tree->node, view->server->layers.fullscreen);
        vela_view_keep_below(view); // Wine's backdrop must not cover the game
        vela_view_keep_in_place(view);
        if (output) {
            struct vela_placement place = vela_output_place(output, vela_output_full_area(output));
            vela_view_configure_size(view, place.width, place.height);
        }
        vela_view_refresh_decoration(view); // fullscreen hides the bar
        if (view->mapped) {
            update_transient_layers(view->server);
            vela_views_raise_transients(view);
        }
        return;
    }

    view->fullscreen = false;
    send_fullscreen(view, false);
    if (view->handle) {
        wlr_foreign_toplevel_handle_v1_set_fullscreen(view->handle, false);
    }
    vela_node_reparent(&view->tree->node, view->server->layers.windows);
    vela_view_keep_below(view);
    if (view->maximized) {
        apply_maximized(view);
    } else if (!vela_snap_is_none(view->snap)) {
        vela_view_apply_snap(view, vela_view_output(view));
    } else {
        restore(view);
    }
    vela_view_refresh_decoration(view);
    if (view->mapped) {
        update_transient_layers(view->server);
        vela_views_raise_transients(view);
    }
}

// ------------------------------------------------------------ minimize --

void vela_view_finish_restore(struct vela_view *view)
{
    if (view->mapped && !view->minimized) {
        vela_node_set_enabled(&view->tree->node, true);
    }
}

// Like Windows: the window disappears, goes to the end of the Alt+Tab order
// and the keyboard moves to the next window. It's restored from the taskbar or
// with Alt+Tab.
void vela_view_set_minimized(struct vela_view *view, bool on)
{
    struct vela_server *server = view->server;
    if (!view->mapped || on == view->minimized) {
        return;
    }
    view->minimized = on;
    if (view->handle) {
        wlr_foreign_toplevel_handle_v1_set_minimized(view->handle, on);
    }
    if (!on) {
        // The real window shows again at the end of the flight
        // (finish_restore); meanwhile it already gets the keyboard.
        vela_focus_view(server, view);
        if (!vela_snapshot_animate(server, view, VELA_SNAPSHOT_RESTORE)) {
            vela_view_finish_restore(view);
        }
        return;
    }
    vela_view_finish_open_animation(view);
    bool was_focused = vela_views_focused(server) == view;
    vela_node_set_enabled(&view->tree->node, false);
    vela_snapshot_animate(server, view, VELA_SNAPSHOT_MINIMIZE);
    if (server->grabbed == view) { // it was being dragged
        vela_snap_end_zone(server, false);
        server->grabbed = NULL;
        server->cursor_mode = VELA_CURSOR_PASSTHROUGH;
    }
    wl_list_remove(&view->link);
    wl_list_insert(server->views.prev, &view->link);
    if (was_focused) {
        vela_view_set_activated(view, false);
        vela_focus_refocus(server);
    }
}

// Brings it to `output` at (x, y) (put back inside the usable area, maximized,
// snapped or fullscreen as before).
static void move_to_output(struct vela_view *view, struct vela_output *output, int x, int y)
{
    // The frame with Vela's bar on top: all inside the usable area.
    int bar = vela_view_title_bar_height(view);
    struct wlr_box geometry = vela_view_geometry(view);
    struct wlr_box frame = vela_view_frame_box(view);
    struct wlr_box outer = { x, y - bar, frame.width, frame.height + bar };
    struct wlr_box fitted = vela_output_fit(output, outer);
    if (!view->maximized && !view->fullscreen && vela_snap_is_none(view->snap)
        && (fitted.width != outer.width || fitted.height != outer.height)) {
        vela_view_configure_size(view, fitted.width, fitted.height - bar);
    }
    vela_node_set_position(&view->tree->node, fitted.x - geometry.x, fitted.y + bar - geometry.y);
    if (view->fullscreen) {
        struct vela_placement place = vela_output_place(output, vela_output_full_area(output));
        vela_view_configure_size(view, place.width, place.height);
        vela_view_keep_in_place(view);
    } else if (view->maximized) {
        apply_maximized(view);
    } else if (!vela_snap_is_none(view->snap)) {
        vela_view_apply_snap(view, output);
    }
}

// ----------------------------------------------------------- life cycle --

// Every listener of a view: they all start disconnected, so all of them
// can be disconnected at the end.
enum { VIEW_LISTENERS = 32 };

static void listeners(struct vela_view *view, struct wl_listener *out[VIEW_LISTENERS])
{
    struct wl_listener *all[VIEW_LISTENERS] = {
        &view->map,
        &view->unmap,
        &view->commit,
        &view->client_commit,
        &view->destroy,
        &view->request_move,
        &view->request_resize,
        &view->request_maximize,
        &view->request_fullscreen,
        &view->request_minimize,
        &view->request_window_menu,
        &view->set_title,
        &view->set_app_id,
        &view->set_parent,
        &view->new_popup,
        &view->decoration_mode,
        &view->decoration_destroy,
        &view->associate,
        &view->dissociate,
        &view->request_configure,
        &view->map_request,
        &view->request_activate,
        &view->set_decorations,
        &view->set_override_redirect,
        &view->request_below,
        &view->request_skip_taskbar,
        &view->handle_activate,
        &view->handle_close,
        &view->handle_maximize,
        &view->handle_minimize,
        &view->handle_fullscreen,
        &view->handle_rectangle,
    };
    memcpy(out, all, sizeof(all));
}

bool vela_view_stays_below(const struct vela_view *view)
{
#if WLR_HAS_XWAYLAND
    return view->x11 && view->x11->below;
#else
    return false;
#endif
}

void vela_view_keep_below(struct vela_view *view)
{
    struct vela_tree *parent = view->tree->node.parent;
    if (!vela_view_stays_below(view) || !parent) {
        return;
    }
    struct vela_node *bottom = wl_container_of(parent->children.next, bottom, link);
    if (bottom != &view->tree->node) {
        vela_node_place_below(&view->tree->node, bottom);
    }
}

void vela_view_init(struct vela_view *view, struct vela_server *server)
{
    view->owner.kind = VELA_OWNER_VIEW;
    view->server = server;
    view->open_tween = (struct vela_tween) { -1.0, 1.0, NULL };
    wl_list_init(&view->link);
    view->tree = vela_tree_create(server->layers.windows);
    view->tree->node.data = &view->owner;
    // All listeners start disconnected: they can always be removed.
    struct wl_listener *all[VIEW_LISTENERS];
    listeners(view, all);
    for (int i = 0; i < VIEW_LISTENERS; ++i) {
        wl_list_init(&all[i]->link);
    }
}

// The window no longer exists for the user: out of the list and of whoever
// remembered it (desktops, snap group, animations, drags).
static void forget(struct vela_view *view)
{
    struct vela_server *server = view->server;
    bool was_focused = vela_views_focused(server) == view;
    vela_workspaces_forget(server, view);
    vela_view_leave_snap_group(view);
    wl_list_remove(&view->link);
    wl_list_init(&view->link);
    vela_snapshot_cancel(server, view);
    vela_focus_forget_view(server, view, was_focused);
}

void vela_view_mapped(struct vela_view *view)
{
    struct vela_server *server = view->server;
    view->mapped = true;
    if (view->xdg) {
        vela_view_update_decoration(view); // the bar is part of the frame to place
    }
    // Initial configure and map must agree on the selected parent output.
    struct vela_output *output = initial_output(view);
    struct wlr_box geometry = vela_view_geometry(view);
    if (output && (view->fullscreen || view->maximized)) {
        struct vela_placement place = vela_output_place(output,
            view->fullscreen ? vela_output_full_area(output) : vela_output_usable_area(output));
        view->target_x = place.x - geometry.x;
        view->target_y = place.y - geometry.y;
        if (geometry.width != place.width || geometry.height != place.height) {
            vela_view_configure_size(view, place.width, place.height);
        }
    } else if (output) {
        // New windows in the center of the usable area, like Windows; never
        // larger than it (some apps remember the size they had on another
        // output or in another session).
        const struct wlr_box area = output->usable;
        struct wlr_box frame = vela_output_fit(output, (struct wlr_box) { 0, 0, geometry.width, geometry.height });
        if (frame.width < geometry.width || frame.height < geometry.height) {
            vela_view_configure_size(view, frame.width, frame.height);
        }
        view->target_x = area.x + (area.width - frame.width) / 2 - geometry.x;
        view->target_y = area.y + (area.height - frame.height) / 2 - geometry.y;
    }
    wl_list_insert(&server->views, &view->link);
    vela_workspaces_view_mapped(server, view);
    struct vela_view *parent = view_parent(view);
    if (parent && parent->mapped) {
        view->workspace = parent->workspace;
        view->sticky = view->sticky || parent->sticky;
    }
    // Mapping a transient on another desktop must not expose its scene node
    // until its workspace is actually selected, including XWayland backdrops.
    vela_node_set_enabled(&view->tree->node, vela_view_on_current_workspace(view));
    // An already-mapped child may need to rejoin the fullscreen layer when
    // its parent maps again after a temporary unmap.
    update_transient_layers(server);
    create_handles(view);
    if (vela_view_stays_below(view)) {
        vela_view_keep_below(view); // no keyboard either: it is a backdrop
    } else {
        vela_focus_new_view(server, view);
    }
    start_open_animation(view);
    // The scene node now has its final output coordinates. Decorations were
    // created before placement, and must be rerasterized at the actual scale.
    if (view->xdg) {
        vela_view_refresh_decoration(view);
    }
    vela_workspaces_announce(server);
}

/** Tear down an unmapped view and revoke any retained window-capture source. */
void vela_view_unmapped(struct vela_view *view)
{
    // Close: the scene still has the last buffers (even if it has already
    // disabled the tree), we photograph them and fade them out. If the window
    // was hidden with a null buffer, the snapshot exists already.
    if (!view->minimized && !view->close_animated) {
        vela_snapshot_animate(view->server, view, VELA_SNAPSHOT_CLOSE);
    }
    view->close_animated = false;
    view->mapped = false;
    view->animating = false;
    if (view->minimized) {
        view->minimized = false;
        vela_node_set_enabled(&view->tree->node, true);
    }
    destroy_handles(view);
    // An unmapped view drops out of server->views; revoke its capture source
    // now so a retained session cannot outlive the view's visible lifetime
    // (and evade the lock-time revocation loop).
    vela_window_capture_destroy(view->capture);
    view->capture = NULL;
    forget(view);
    update_transient_layers(view->server);
    vela_workspaces_announce(view->server);
}

void vela_view_committed(struct vela_view *view)
{
    if (view->mapped) {
        vela_view_keep_in_place(view);
    }
    if (view->xdg) {
        vela_view_update_decoration(view); // the server-side bar starts (or ends) with this commit
    } else {
        vela_view_refresh_decoration(view); // size and scale may have changed
    }
}

void vela_view_destroy(struct vela_view *view)
{
    vela_decoration_destroy(view->decoration);
    view->decoration = NULL;
    destroy_handles(view);
    vela_window_capture_destroy(view->capture);
    view->capture = NULL;
    view->mapped = false;
    forget(view);
    update_transient_layers(view->server);
    struct wl_listener *all[VIEW_LISTENERS];
    listeners(view, all);
    for (int i = 0; i < VIEW_LISTENERS; ++i) {
        wl_list_remove(&all[i]->link);
    }
    if (view->surface_node) {
        vela_node_destroy(&view->surface_node->node);
    }
    vela_node_destroy(&view->tree->node);
    free(view);
}

// -------------------------------------------------------- Wayland windows --

static void handle_map(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, map);
    vela_view_mapped(view);
}

static void handle_unmap(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, unmap);
    vela_view_unmapped(view);
}

static void handle_commit(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, commit);
    struct wlr_xdg_toplevel *xdg = view->xdg;
    if (xdg->base->initial_commit) {
        // First commit: tell the client what we can do and let it choose its
        // size (0x0), unless it has already asked to start maximized or
        // fullscreen.
        wlr_xdg_toplevel_set_wm_capabilities(xdg,
            WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE | WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN
                | WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE | WLR_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU);
        // Vela draws the bar, if the app doesn't want one of its own (§9.1).
        if (view->xdg_decoration) {
            wlr_xdg_toplevel_decoration_v1_set_mode(view->xdg_decoration, decoration_mode(view->xdg_decoration));
        }
        if (xdg->requested.fullscreen) {
            vela_view_set_fullscreen(view, true);
        } else if (xdg->requested.maximized) {
            vela_view_set_maximized(view, true, true);
        } else {
            wlr_xdg_toplevel_set_size(xdg, 0, 0);
        }
        return;
    }
    vela_view_committed(view);
}

// The app is about to remove its content with a null buffer (window hidden):
// photograph it before the commit applies and the scene loses the buffer.
static void handle_client_commit(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, client_commit);
    const struct wlr_surface_state *pending = &view->xdg->base->surface->pending;
    if (view->mapped && !view->minimized && !view->close_animated && (pending->committed & WLR_SURFACE_STATE_BUFFER)
        && !pending->buffer) {
        view->close_animated = vela_snapshot_animate(view->server, view, VELA_SNAPSHOT_CLOSE);
    }
}

static void handle_destroy(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, destroy);
    vela_view_destroy(view);
}

static void handle_request_move(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_move);
    vela_interact_begin(view->server, view, VELA_CURSOR_MOVE, 0, false);
}

static void handle_request_resize(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;
    vela_interact_begin(view->server, view, VELA_CURSOR_RESIZE, event->edges, false);
}

static void handle_request_maximize(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_maximize);
    if (view->xdg->base->initialized) {
        vela_view_set_maximized(view, view->xdg->requested.maximized, true);
    }
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_fullscreen);
    if (view->xdg->base->initialized) {
        vela_view_set_fullscreen(view, view->xdg->requested.fullscreen);
    }
}

static void handle_request_minimize(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_minimize);
    vela_view_set_minimized(view, true);
}

// Apps with their own bar (GTK, libadwaita) ask for the window menu with a
// right click on their bar.
static void handle_request_window_menu(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, request_window_menu);
    struct wlr_xdg_toplevel_show_window_menu_event *event = data;
    vela_window_menu_show(view->server, view, view->tree->node.x + event->x, view->tree->node.y + event->y, false);
}

static void handle_set_title(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, set_title);
    vela_view_title_changed(view);
}

static void handle_set_app_id(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, set_app_id);
    vela_view_app_id_changed(view);
}

static void handle_set_parent(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, set_parent);
    vela_view_parent_changed(view);
}

static void handle_new_popup(struct wl_listener *listener, void *data)
{
    struct vela_view *view = wl_container_of(listener, view, new_popup);
    vela_popup_create(data, view->tree, &view->owner);
}

static void handle_new_toplevel(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, new_toplevel);
    struct wlr_xdg_toplevel *xdg = data;
    struct vela_view *view = calloc(1, sizeof(*view));
    vela_view_init(view, server);
    view->xdg = xdg;
    view->surface_node = vela_surface_node_create(view->tree, xdg->base->surface);
    xdg->base->data = view; // to get back to the parent window

    struct wlr_surface *surface = xdg->base->surface;
    vela_listen(&surface->events.map, &view->map, handle_map);
    vela_listen(&surface->events.unmap, &view->unmap, handle_unmap);
    vela_listen(&surface->events.commit, &view->commit, handle_commit);
    vela_listen(&surface->events.client_commit, &view->client_commit, handle_client_commit);
    vela_listen(&xdg->events.destroy, &view->destroy, handle_destroy);
    vela_listen(&xdg->events.request_move, &view->request_move, handle_request_move);
    vela_listen(&xdg->events.request_resize, &view->request_resize, handle_request_resize);
    vela_listen(&xdg->events.request_maximize, &view->request_maximize, handle_request_maximize);
    vela_listen(&xdg->events.request_fullscreen, &view->request_fullscreen, handle_request_fullscreen);
    vela_listen(&xdg->events.request_minimize, &view->request_minimize, handle_request_minimize);
    vela_listen(&xdg->events.request_show_window_menu, &view->request_window_menu, handle_request_window_menu);
    vela_listen(&xdg->events.set_title, &view->set_title, handle_set_title);
    vela_listen(&xdg->events.set_app_id, &view->set_app_id, handle_set_app_id);
    vela_listen(&xdg->events.set_parent, &view->set_parent, handle_set_parent);
    vela_listen(&xdg->base->events.new_popup, &view->new_popup, handle_new_popup);
}

// -------------------------------------------------------- active windows --

struct vela_view *vela_views_focused(struct vela_server *server)
{
    struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
    if (!focused) {
        return NULL;
    }
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (vela_view_surface(view) == focused) {
            return view;
        }
    }
    return NULL;
}

struct vela_view *vela_views_system_prompt(struct vela_server *server)
{
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->mapped && !view->minimized && vela_view_on_current_workspace(view)
            && vela_view_is_system_prompt(view)) {
            return view;
        }
    }
    return NULL;
}

// An app asks to bring one of its windows forward (a link opened in an already
// running Firefox, a click on a notification).
static void handle_request_activation(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, request_activation);
    struct wlr_xdg_activation_v1_request_activate_event *event = data;
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (vela_view_surface(view) == event->surface) {
            vela_focus_new_view(server, view);
            return;
        }
    }
}

/** Reject foreign-toplevel capture during lock; accept only visible-session requests. */
static void handle_capture_request(struct wl_listener *listener, void *data)
{
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request = data;
    struct vela_view *view = request->toplevel_handle->data;
    if (!view || view->server->locked) {
        // An isolated window source can bypass visible composition; refuse
        // access to any window hidden behind the lock screen.
        return;
    }
    // The current look, even if minimized or covered by other windows: the
    // renderer draws the window alone.
    struct vela_server *server = view->server;
    if (!view->capture) {
        view->capture
            = vela_window_capture_create(server->scene, &view->tree->node, view, server->renderer, server->allocator);
    }
    struct wlr_ext_image_capture_source_v1 *source = vela_window_capture_source(view->capture);
    if (source) {
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request, source);
    }
}

/** Revoke active foreign-toplevel capture sources when locking the session. */
void vela_views_revoke_capture_on_lock(struct vela_server *server)
{
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        vela_window_capture_destroy(view->capture);
        view->capture = NULL;
    }
}

void vela_views_init(struct vela_server *server)
{
    struct wl_display *display = server->display;
    wl_list_init(&server->views);
    server->xdg_shell = wlr_xdg_shell_create(display, 5);
    vela_listen(&server->xdg_shell->events.new_toplevel, &server->new_toplevel, handle_new_toplevel);

    // The window list for the taskbar, with activate/minimize/close.
    server->foreign_toplevels = wlr_foreign_toplevel_manager_v1_create(display);

    // Output and window capture (Alt+Tab previews, and later screen sharing):
    // the standard ext-* protocols.
    server->ext_toplevels = wlr_ext_foreign_toplevel_list_v1_create(display, 1);
    wlr_ext_image_copy_capture_manager_v1_create(display, 1);
    if (!vela_output_capture_manager_create(server)) {
        wlr_log(WLR_ERROR, "Cannot create output capture manager");
    }
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *capture
        = wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(display, 1);
    vela_listen(&capture->events.new_request, &server->new_capture_request, handle_capture_request);

    // Vela's title bar for the apps that accept it (§9.1).
    struct wlr_xdg_decoration_manager_v1 *decorations = wlr_xdg_decoration_manager_v1_create(display);
    vela_listen(&decorations->events.new_toplevel_decoration, &server->new_decoration, handle_new_decoration);

    struct wlr_xdg_activation_v1 *activation = wlr_xdg_activation_v1_create(display);
    vela_listen(&activation->events.request_activate, &server->request_activation, handle_request_activation);
}

void vela_views_finish(struct vela_server *server)
{
    vela_unlisten(&server->new_toplevel);
    vela_unlisten(&server->new_capture_request);
    vela_unlisten(&server->new_decoration);
    vela_unlisten(&server->request_activation);
    if (server->output_check) {
        wl_event_source_remove(server->output_check);
        server->output_check = NULL;
    }
    free(server->seen_outputs);
    server->seen_outputs = NULL;
}

// ------------------------------------------------------------- outputs --

void vela_views_usable_changed(struct vela_server *server, struct vela_output *output)
{
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->fullscreen || vela_view_output(view) != output) {
            continue;
        }
        if (view->maximized) {
            apply_maximized(view);
        } else if (!vela_snap_is_none(view->snap)) {
            vela_view_apply_snap(view, output);
        }
    }
}

bool vela_views_fullscreen_on(struct vela_server *server, struct vela_output *output)
{
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->mapped && view->fullscreen && !view->minimized && vela_view_on_current_workspace(view)
            && vela_view_output(view) == output) {
            return true;
        }
    }
    return false;
}

static const struct vela_seen_output *seen_output(const struct vela_seen_output *list, int count, const char *name)
{
    for (int i = 0; i < count; ++i) {
        if (strcmp(list[i].name, name) == 0) {
            return &list[i];
        }
    }
    return NULL;
}

static int compare_seen(const void *a, const void *b)
{
    return strcmp(((const struct vela_seen_output *)a)->name, ((const struct vela_seen_output *)b)->name);
}

void vela_views_check_outputs(struct vela_server *server)
{
    // The current outputs, by name (as they will be remembered next time).
    int count = wl_list_length(&server->outputs);
    struct vela_seen_output *now = calloc((size_t)(count ? count : 1), sizeof(*now));
    int n = 0;
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        if (output->wlr->enabled && wlr_output_layout_get(server->output_layout, output->wlr)
            && !seen_output(now, n, output->wlr->name)) {
            snprintf(now[n].name, sizeof(now[n].name), "%s", output->wlr->name);
            now[n++].box = vela_output_box(output);
        }
    }
    qsort(now, (size_t)n, sizeof(*now), compare_seen);

    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (!view->mapped) {
            continue;
        }
        struct wlr_box frame = vela_view_frame_box(view);
        double cx = frame.x + frame.width / 2.0;
        double cy = frame.y + frame.height / 2.0;
        // The output it was moved away from is back: it returns there.
        const struct vela_seen_output *home = view->home_output[0] ? seen_output(now, n, view->home_output) : NULL;
        if (home) {
            struct vela_output *back = vela_output_named(server, view->home_output);
            if (back) {
                wlr_log(WLR_INFO, "%s connected again: \"%s\" goes back there", view->home_output,
                    vela_view_title(view));
                view->home_output[0] = '\0';
                move_to_output(view, back, home->box.x + view->home_x, home->box.y + view->home_y);
            }
            continue;
        }
        if (vela_output_at(server, cx, cy)) {
            continue;
        }
        // Its output is gone: remember which it was (from last time's
        // positions) and move to the nearest output.
        const char *from = NULL;
        for (int i = 0; i < server->seen_output_count; ++i) {
            const struct wlr_box *box = &server->seen_outputs[i].box;
            if (cx >= box->x && cx < box->x + box->width && cy >= box->y && cy < box->y + box->height) {
                from = server->seen_outputs[i].name;
                view->home_x = frame.x - box->x;
                view->home_y = frame.y - box->y;
            }
        }
        double nx = 0.0;
        double ny = 0.0;
        wlr_output_layout_closest_point(server->output_layout, NULL, cx, cy, &nx, &ny);
        struct vela_output *target = vela_output_at(server, nx, ny);
        if (!target && !wl_list_empty(&server->outputs)) {
            target = wl_container_of(server->outputs.next, target, link);
        }
        if (!target || !wlr_output_layout_get(server->output_layout, target->wlr)) {
            continue; // no output: wait for one to come back
        }
        if (from && !view->home_output[0]) {
            snprintf(view->home_output, sizeof(view->home_output), "%s", from);
        }
        struct wlr_box to = vela_output_box(target);
        wlr_log(WLR_INFO, "%s disconnected: \"%s\" moves to %s", from ? from : "An output", vela_view_title(view),
            target->wlr->name);
        // At the same relative point, if it fits.
        move_to_output(view, target, to.x + (from ? view->home_x : 0), to.y + (from ? view->home_y : 0));
    }
    free(server->seen_outputs);
    server->seen_outputs = now;
    server->seen_output_count = n;
}

static void handle_output_check(void *data)
{
    struct vela_server *server = data;
    server->output_check = NULL;
    vela_views_check_outputs(server);
}

void vela_views_check_outputs_later(struct vela_server *server)
{
    if (!server->output_check) {
        server->output_check = wl_event_loop_add_idle(server->loop, handle_output_check, server);
    }
}
