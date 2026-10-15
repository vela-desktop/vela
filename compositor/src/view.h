// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_VIEW_H
#define VELA_VIEW_H

// A window: of a Wayland app (xdg-shell) or an X11 one (Xwayland). Everything
// Vela does with windows (animations, snap, taskbar, Alt+Tab) goes through
// here; the few things that depend on the kind are in the "to the app"
// functions (view.c for Wayland, xwayland.c for X11).
//
// Windows are in vela_server.views in order of recent use: the first is the
// active one. A window is born with its surface and dies with it; the list
// holds it only while it's "mapped" (visible to the user).

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

#include "motion.h"
#include "snap.h"

struct vela_decoration;
struct vela_output;
struct vela_server;
struct vela_surface_node;
struct vela_tree;
struct vela_window_capture;
struct wlr_ext_foreign_toplevel_handle_v1;
struct wlr_ext_image_capture_source_v1;
struct wlr_foreign_toplevel_handle_v1;
struct wlr_surface;
struct wlr_xdg_toplevel;
struct wlr_xdg_toplevel_decoration_v1;
struct wlr_xwayland_surface;

// Who owns a scene tree (its vela_node.data): a window or a shell piece. It's
// the first field of their structs, so the result of vela_scene_at leads back
// to them.
enum vela_owner_kind {
    VELA_OWNER_VIEW,
    VELA_OWNER_LAYER, // struct vela_layer_surface (layer.h)
};

struct vela_owner {
    enum vela_owner_kind kind;
};

struct vela_view {
    struct vela_owner owner; // first field: the tree's vela_node.data
    struct wl_list link; // vela_server.views, while mapped
    struct vela_server *server;
    struct wlr_xdg_toplevel *xdg; // one of the two
    struct wlr_xwayland_surface *x11;
    // The tree's origin: the surface's (not the geometry's, which can have a
    // shadow margin).
    struct vela_tree *tree;
    struct vela_surface_node *surface_node;

    bool mapped;
    bool activated; // the active window (keyboard)
    bool maximized;
    bool fullscreen;
    bool minimized;
    struct vela_snap snap; // where it is, if snapped (snap.h)
    // Snap group, like Windows 11: the windows arranged together with Snap
    // Assist; the taskbar shows them and brings them forward together. 0:
    // none. A window leaves by being moved away (or closed).
    uint32_t snap_group;
    // Virtual desktops (workspace.c): the window's, or all.
    int workspace;
    bool sticky;
    uint64_t map_serial; // opening order, for the taskbar
    struct wlr_box restore; // position and size before maximizing or snapping
    // The output it was moved away from because it was unplugged (empty: none)
    // and where it was there: when the output comes back, so does the window.
    char home_output[64];
    int home_x;
    int home_y;
    struct wlr_box taskbar_rect; // its taskbar button (global), if known

    // How the taskbar sees and commands this window (foreign-toplevel). It
    // exists only while the window is mapped and on the current desktop.
    struct wlr_foreign_toplevel_handle_v1 *handle;
    // The same window in the ext-foreign-toplevel-list protocol: it has a
    // unique identifier and is used to capture its image (Alt+Tab). It exists
    // while the window is mapped.
    struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle;
    // To capture its image (Alt+Tab previews): our renderer draws it alone.
    struct vela_window_capture *capture;
    // Vela's title bar (decoration.h), if the window uses it, and the app's
    // xdg-decoration request.
    struct vela_decoration *decoration;
    struct wlr_xdg_toplevel_decoration_v1 *xdg_decoration;

    // Open animation.
    bool animating;
    bool close_animated; // close snapshot already taken
    struct vela_tween open_tween;
    int open_frames;
    double target_x;
    double target_y;

    // X11: size asked of the app (0: its own), last geometry sent and the one
    // the app asked for before appearing.
    int x11_width;
    int x11_height;
    struct wlr_box x11_sent;
    struct wlr_box x11_initial;
    // Wine's private X11 style reports window mode changes that some games
    // do not mirror to _NET_WM_STATE_FULLSCREEN (e.g. fullscreen popup -> captioned).
    uint32_t x11_wine_style;
    unsigned int x11_wine_style_cookie;
    bool x11_wine_style_known;
    bool x11_wine_style_pending;
    bool x11_wine_style_dirty;
    // Only a fullscreen exit initiated by a Wine style change arms re-entry.
    bool x11_wine_restore_fullscreen;
    // The _NET_WM_STATE the app set before mapping, read at its MapRequest.
    unsigned int x11_initial_state_cookie;
    bool x11_initial_state_pending;
    bool x11_start_fullscreen;
    bool x11_start_maximized;

    struct wl_listener map_request; // X11
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener client_commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener request_minimize;
    struct wl_listener request_window_menu;
    struct wl_listener set_title;
    struct wl_listener set_app_id;
    struct wl_listener set_parent;
    struct wl_listener new_popup;
    struct wl_listener decoration_mode;
    struct wl_listener decoration_destroy;
    // X11 only.
    struct wl_listener associate;
    struct wl_listener dissociate;
    struct wl_listener request_configure;
    struct wl_listener request_activate;
    struct wl_listener set_decorations;
    // The taskbar's requests through the handle.
    struct wl_listener handle_activate;
    struct wl_listener handle_close;
    struct wl_listener handle_maximize;
    struct wl_listener handle_minimize;
    struct wl_listener handle_fullscreen;
    struct wl_listener handle_rectangle;
};

// The Wayland window protocols (xdg-shell, xdg-decoration, foreign-toplevel,
// xdg-activation) and the vela_server.views list.
void vela_views_init(struct vela_server *server);
void vela_views_finish(struct vela_server *server);
// Revoke existing window capture sources before hiding the desktop on lock.
void vela_views_revoke_capture_on_lock(struct vela_server *server);

// The active window (the one with the keyboard), or NULL.
struct vela_view *vela_views_focused(struct vela_server *server);
// A visible system dialog waiting for an answer, or NULL.
struct vela_view *vela_views_system_prompt(struct vela_server *server);

// -------------------------------------------------------------- geometry --

// The visible part, relative to the surface origin (Wayland apps can draw a
// shadow margin around it), Vela's bar included.
struct wlr_box vela_view_geometry(const struct vela_view *view);
// The window's visible frame (Vela's bar included), in global logical
// coordinates.
struct wlr_box vela_view_frame_box(struct vela_view *view);
// The output under the window's center, or the cursor's.
struct vela_output *vela_view_output(struct vela_view *view);
// Where it "goes" when minimized: its taskbar button.
struct wlr_box vela_view_minimize_target(struct vela_view *view);
// Where it returns when leaving maximized or fullscreen.
struct wlr_box vela_view_restore_box(struct vela_view *view);
// Height of Vela's title bar (logical), 0 if the window has none or is
// fullscreen.
int vela_view_title_bar_height(const struct vela_view *view);

// ---------------------------------------------------------------- states --

// animate: false when the window leaves maximized because it's being dragged
// (it already follows the cursor).
void vela_view_set_maximized(struct vela_view *view, bool on, bool animate);
void vela_view_set_fullscreen(struct vela_view *view, bool on);
// Like Windows: the window disappears, goes to the end of the Alt+Tab order
// and the keyboard moves to the next one.
void vela_view_set_minimized(struct vela_view *view, bool on);
// End of the restore animation: the real window shows again.
void vela_view_finish_restore(struct vela_view *view);
void vela_view_set_activated(struct vela_view *view, bool on); // for the app and for the taskbar
// Maximized, snapped or fullscreen: realigns it to its output.
void vela_view_keep_in_place(struct vela_view *view);
void vela_view_finish_open_animation(struct vela_view *view);
// The open animations at `now_ms` (the frame's time), retired at `wall_ms`:
// true while some remain.
bool vela_views_tick(struct vela_server *server, double now_ms, double wall_ms);
// Rounded corners and shadow (docs/renderer.md §8), like Windows 11: not when
// maximized, fullscreen or snapped, nor for apps that draw their own shadow
// and borders (margins outside the geometry, such as GTK).
void vela_view_update_shape(struct vela_view *view);
// Creates or removes Vela's bar as the window asks, or realigns it to the
// window.
void vela_view_update_decoration(struct vela_view *view);
void vela_view_refresh_decoration(struct vela_view *view);
// The taskbar shows only the current desktop's windows.
void vela_view_show_in_taskbar(struct vela_view *view, bool on);

// ------------------------------------------------------------ to the app --

struct wlr_surface *vela_view_surface(const struct vela_view *view); // NULL until X11 is associated
bool vela_view_configurable(const struct vela_view *view); // can already receive sizes and states
const char *vela_view_title(const struct vela_view *view); // never NULL
const char *vela_view_app_id(const struct vela_view *view); // never NULL (X11: the class)
// A system dialog waiting for an answer (keyring, administrator password,
// GnuPG PIN): it stays above normal windows.
bool vela_view_is_system_prompt(const struct vela_view *view);
bool vela_view_resizable(const struct vela_view *view); // not if min = max
pid_t vela_view_pid(const struct vela_view *view); // the process (for "End task")
// Size of the whole frame (bar included); 0x0: the app chooses.
void vela_view_configure_size(struct vela_view *view, int width, int height);
void vela_view_send_maximized(struct vela_view *view, bool on);
void vela_view_send_tiled(struct vela_view *view, uint32_t edges);
void vela_view_close(struct vela_view *view);
// X11: apps must know where the window is on screen (to place their own
// menus). Called on every frame.
void vela_view_sync_x11(struct vela_view *view);

// ------------------------------------------------------- outputs and scene --

// An output's usable area changed: the maximized and snapped windows on it are
// rearranged.
void vela_views_usable_changed(struct vela_server *server, struct vela_output *output);
// A visible fullscreen app on that output (VRR "games").
bool vela_views_fullscreen_on(struct vela_server *server, struct vela_output *output);
// Outputs plugged and unplugged: the windows of an output that goes away move
// to another, and come back when it does. "later": after everything has
// settled (an output that goes away leaves the layout before being destroyed),
// on the next loop iteration.
void vela_views_check_outputs_later(struct vela_server *server);
void vela_views_check_outputs(struct vela_server *server);

// --------------------------------------------- for view.c and xwayland.c --

// The common life cycle: map, unmap, commit (view.c).
void vela_view_init(struct vela_view *view, struct vela_server *server);
void vela_view_mapped(struct vela_view *view);
void vela_view_unmapped(struct vela_view *view);
void vela_view_committed(struct vela_view *view);
void vela_view_destroy(struct vela_view *view);
void vela_view_title_changed(struct vela_view *view);
void vela_view_app_id_changed(struct vela_view *view);
void vela_view_parent_changed(struct vela_view *view);
// X11 apps (xwayland.c).
void vela_xwayland_init(struct vela_server *server);
void vela_xwayland_finish(struct vela_server *server);
void vela_xwayland_sync(struct vela_server *server); // on every frame: X11 apps know where they are

#endif
