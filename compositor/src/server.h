// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SERVER_H
#define VELA_SERVER_H

// The compositor: Wayland display, backend, renderer, scene and everything
// that lives as long as the session. main.c creates it and destroys it at the
// end.
//
// The struct holds the global objects and pointers to the subsystems; each
// subsystem has its module, which creates and destroys it (server.c says in
// which order).

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

#include "process.h"
#include "scene/ready.h"
#include "shell.h"

struct vela_a11y;
struct vela_commands;
struct vela_icons;
struct vela_input;
struct vela_interaction;
struct vela_layer_surface;
struct vela_snapping;
struct vela_switcher;
struct vela_lock;
struct vela_text;
struct vela_output;
struct vela_output_previews;
struct vela_renderer;
struct vela_scene;
struct vela_tree;
struct vela_view;
struct vela_vulkan;
struct vela_workspaces;
struct wlr_allocator;
struct wlr_compositor;
struct wlr_ext_foreign_toplevel_list_v1;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_xdg_shell;
struct wlr_xwayland;
struct wlr_backend;
struct wlr_cursor;
struct wlr_output_layout;
struct wlr_renderer;
struct wlr_seat;
struct wlr_layer_shell_v1;
struct wlr_output_manager_v1;
struct wlr_session;
struct wlr_xcursor_manager;

// The scene layers, bottom to top. Creation order is stacking order.
struct vela_layers {
    struct vela_tree *background;
    struct vela_tree *bottom;
    struct vela_tree *windows;
    // The windows of the desktop being left, while they slide away: above the
    // windows and below the panels.
    struct vela_tree *windows_out;
    struct vela_tree *top;
    struct vela_tree *fullscreen;
    // The "top" layer panels that are summoned (Start menu, quick settings,
    // Run...): like Windows, they appear above a fullscreen game too, which
    // instead covers the taskbar.
    struct vela_tree *top_above_fullscreen;
    struct vela_tree *x11_popups; // menus and tooltips of X11 apps
    struct vela_tree *overlay;
    struct vela_tree *drag; // the icon of what's being dragged between apps
    struct vela_tree *lock; // lock screen, above everything
};

// Where an output was last time (view.c: windows go back to their output when
// it comes back).
struct vela_seen_output {
    char name[64];
    struct wlr_box box;
};

// What the pointer does: nothing special, moving or resizing a window.
enum vela_cursor_mode {
    VELA_CURSOR_PASSTHROUGH,
    VELA_CURSOR_MOVE,
    VELA_CURSOR_RESIZE,
};

struct vela_server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_backend *backend;
    struct wlr_session *session; // only in the real session (DRM): VT switching
    // Vela's renderer (docs/renderer.md): our own Vulkan device, the renderer
    // (also a wlr_renderer for wlroots) and the GBM allocator for output,
    // cursor and capture buffers.
    struct vela_vulkan *vulkan;
    struct vela_renderer *renderer; // belongs to wlroots: see vela_server_destroy
    struct wlr_renderer *wlr_renderer; // the same, as wlroots sees it
    struct wlr_allocator *allocator;
    struct wlr_output_layout *output_layout;
    struct vela_scene *scene;
    struct wl_list outputs; // vela_output.link
    // Variable refresh rate (VRR): 0 never, 1 with a fullscreen app (games), 2
    // always. vela.conf "variable-refresh".
    int vrr_mode;
    bool nested; // inside another session (Wayland or X11 window)
    bool unsupported_gpu; // renderer initialization failed; supervisor must not retry
    char socket_name[64]; // our WAYLAND_DISPLAY
    // Exact XDG_RUNTIME_DIR inode inherited from the supervisor; -1 when
    // running nested/headless without supervision.
    int runtime_dir_fd;

    struct vela_layers layers;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_list layer_surfaces; // vela_layer_surface.link (layer.h)
    struct wl_listener new_layer_surface;
    struct wlr_seat *seat;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_manager;
    enum vela_cursor_mode cursor_mode;
    struct vela_input *input; // devices, keyboards, gestures (input.h)
    struct vela_a11y *a11y; // night light, filters, magnifier, sticky keys (a11y.h)
    struct vela_lock *lock; // screen lock and inactivity (lock.h)
    // Locked (ext-session-lock-v1): only the lock program shows.
    bool locked;

    // The theme chosen in Settings ("Choose your mode"), sent by the shell:
    // light shell (acrylic tint) and light apps (title bars).
    bool light_shell;
    bool light_apps;
    // The desktop wallpaper's tint (Mica, docs/renderer.md §9.4): the average
    // color, in sRGB, sent by the shell. The version changes with it and with
    // the theme: title bars redraw.
    bool has_wallpaper_tint;
    float wallpaper_tint[3];
    uint32_t wallpaper_tint_version;
    // Title bar text and icons, loaded on first use (decoration.c);
    // text_loaded: tried, even if there is no font.
    struct vela_text *text;
    bool text_loaded;
    struct vela_icons *icons;
    // Animation time: the moment the frame being prepared will become light
    // (docs/renderer.md §4.2). It never goes back, even when different outputs
    // predict different moments; 0 before the first.
    double animation_now_ms;
    struct wl_list snapshot_animations; // the snapshot animations (snapshot.h)

    // The windows (view.h): in order of recent use, the first is the active
    // one; the protocols that bring them.
    struct wl_list views; // vela_view.link
    struct wlr_compositor *compositor;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_foreign_toplevel_manager_v1 *foreign_toplevels;
    struct wlr_ext_foreign_toplevel_list_v1 *ext_toplevels;
    struct wl_listener new_toplevel;
    struct wl_listener new_capture_request;
    struct wl_listener new_decoration;
    struct wl_listener request_activation;
    struct wlr_xwayland *xwayland; // NULL without Xwayland
    struct wl_listener xwayland_ready;
    struct wl_listener xwayland_new_surface;
    struct wl_listener xwayland_shell_surface;
    uint32_t xwayland_wake_atom; // the root property that wakes the X window manager (xwayland.c)
    uint32_t xwayland_wine_style_atom; // _WINE_HWND_STYLE, interned on Xwayland ready
    struct wl_event_source *output_check;
    struct vela_seen_output *seen_outputs;
    int seen_output_count;
    // The window the pointer is moving or resizing (cursor_mode), or NULL.
    struct vela_view *grabbed;
    struct vela_workspaces *workspaces; // virtual desktops (workspace.h)
    // The keyboard for shell pieces (focus.h): the one that has it and the one
    // it returns to when the one above closes.
    struct vela_layer_surface *focused_layer;
    struct vela_layer_surface *previous_layer;
    struct vela_interaction *interaction; // the pointer on windows (interact.h)
    struct vela_switcher *switcher; // Alt+Tab (switcher.h)
    struct vela_snapping *snapping; // preview, assist, snap layouts (snap.h)

    // App commits held until their GPU has finished (§7.3).
    struct vela_ready ready;
    struct vela_child shell; // the launched and supervised shell (shell.h)
    struct vela_child polkit_agent; // the polkit agent (polkit.h)
    struct vela_commands *commands; // the command socket (command.h)
    // wlr-output-management (output_manager.h).
    struct wlr_output_manager_v1 *output_manager;
    struct vela_output_previews *output_previews;
    struct wl_listener output_manager_apply;
    struct wl_listener output_manager_test;
    struct wl_listener new_output;
    struct wl_listener layout_change;
};

// Creates the display, backend, renderer and protocols. NULL if something
// essential is missing (the reason is already in the log).
struct vela_server *vela_server_create(bool *unsupported_gpu);

// Opens the Wayland socket, starts the backend and launches `startup_command`
// (the shell, relaunched when it fails; NULL or empty: nothing).
bool vela_server_start(struct vela_server *server, const char *startup_command);

// The event loop, until exit.
void vela_server_run(struct vela_server *server);

// Closes the clients and frees everything.
void vela_server_destroy(struct vela_server *server);


// An output goes away: panels closed, previews and magnifier off, the lock no
// longer waits for its frame.
void vela_server_output_destroyed(struct vela_server *server, struct vela_output *output);

// Before every frame: animations advance to the moment the frame will become
// light (docs/renderer.md §4.2).
void vela_server_animate(struct vela_server *server, int64_t present_ns);

// On every delivered frame (the lock waits for black on every output).
void vela_server_output_rendered(struct vela_server *server, struct vela_output *output);

// A frame on every output (something animates or changed).
void vela_server_schedule_frames(struct vela_server *server);



#endif
