// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "server.h"

#include "a11y.h"
#include "command.h"
#include "config.h"
#include "error_screen.h"
#include "input.h"
#include "interact.h"
#include "layer.h"
#include "listen.h"
#include "lock.h"
#include "motion.h"
#include "output.h"
#include "output_manager.h"
#include "path_security.h"
#include "polkit.h"
#include "render/renderer.h"
#include "scene/effects.h"
#include "scene/frame.h"
#include "scene/scene.h"
#include "session.h"
#include "snap.h"
#include "snapshot.h"
#include "switcher.h"
#include "text.h"
#include "icons.h"
#include "util.h"
#include "view.h"
#include "workspace.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/config.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>
#if WLR_HAS_X11_BACKEND
#include <wlr/backend/x11.h>
#endif
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif

static int handle_terminate(int signal, void *data)
{
    wl_display_terminate(data);
    return 0;
}

// For damage debugging: `kill -USR1` redraws everything from scratch. If the
// image changes, the damage had left stale pixels.
static int handle_redraw_all(int signal, void *data)
{
    struct vela_server *server = data;
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        vela_output_frame_reset_damage(output->frame);
    }
    return 0;
}

static void find_windowed(struct wlr_backend *child, void *data)
{
    bool windowed = wlr_backend_is_wl(child);
#if WLR_HAS_X11_BACKEND
    windowed = windowed || wlr_backend_is_x11(child);
#endif
    if (windowed) {
        *(bool *)data = true;
    }
}

static void handle_new_output(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, new_output);
    vela_output_create(server, data);
}

// Every change (output plugged, moved, new mode) is reported to configuration
// programs.
static void handle_layout_change(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, layout_change);
    vela_output_manager_update(server);
    vela_lock_update_layout(server->lock);
    vela_views_check_outputs_later(server);
}

// The frame loop must wake at the right moment even with the CPU busy (a
// build, a game): realtime scheduling, at low priority, for the main thread,
// like KWin. What Vela launches (shell, apps) doesn't inherit it. Needs
// RLIMIT_RTPRIO or CAP_SYS_NICE; VELA_REALTIME=0 turns it off.
// VELA_DEBUG_ANIMATION=1 (motion.h).
static void log_animation(const char *line)
{
    wlr_log(WLR_INFO, "animation: %s", line);
}

static void make_realtime(void)
{
    if (vela_env_off("VELA_REALTIME")) {
        return;
    }
    struct sched_param param = { .sched_priority = vela_min(10, sched_get_priority_max(SCHED_RR)) };
    if (sched_setscheduler(0, SCHED_RR | SCHED_RESET_ON_FORK, &param) == 0) {
        wlr_log(WLR_INFO, "Main thread is realtime (SCHED_RR, priority %d)", param.sched_priority);
    } else {
        wlr_log(WLR_INFO, "No realtime scheduling (%s): frames may be late under load", strerror(errno));
    }
}

// Display, backend, renderer and scene; false if something essential is
// missing (the reason is in the log).
static bool init(struct vela_server *server)
{
    // vela.conf with the English names, if it still has the old Italian ones.
    vela_config_migrate();
    wl_list_init(&server->outputs);
    wl_list_init(&server->new_output.link);
    wl_list_init(&server->layout_change.link);
    vela_child_init(&server->shell, "Shell", SIGKILL, -1);
    // SIGTERM, not SIGKILL: the agent unregisters from polkitd before
    // exiting. Exit code 2: another agent already owns this session.
    vela_child_init(&server->polkit_agent, "Polkit agent", SIGTERM, 2);
    vela_snapshots_init(server);
    server->interaction = vela_interaction_create();
    server->switcher = vela_switcher_create();
    server->snapping = vela_snapping_create();
    // Vela's accent until the shell sends the user's (Theme.qml's default).
    server->accent = (struct wlr_render_color) { 0x5b / 255.0f, 0x8c / 255.0f, 1.0f, 1.0f };
    server->vrr_mode = 1;
    server->display = wl_display_create();
    server->loop = wl_display_get_event_loop(server->display);
    struct wl_display *display = server->display;

    // Picks the backend by itself: DRM/KMS from a VT, or a Wayland window when
    // launched inside another session (such as KDE) for testing.
    server->backend = wlr_backend_autocreate(server->loop, &server->session);
    if (!server->backend) {
        wlr_log(WLR_ERROR, "Can't create the backend");
        return false;
    }
    wlr_multi_for_each_backend(server->backend, find_windowed, &server->nested);

    // Vela's renderer (docs/renderer.md): Vulkan 1.4 only, on the device of
    // the GPU driving the outputs. Without it Vela doesn't start and the log
    // says why.
    server->vulkan = vela_vulkan_create(wlr_backend_get_drm_fd(server->backend));
    if (server->vulkan) {
        server->renderer = vela_renderer_create(server->vulkan);
    }
    if (!server->renderer) {
        wlr_log(WLR_ERROR, "Vela needs a GPU with Vulkan 1.4 and dmabuf support: can't draw");
        server->unsupported_gpu = true;
        vela_error_screen_no_vulkan(wlr_backend_get_drm_fd(server->backend));
        return false;
    }
    server->wlr_renderer = vela_renderer_wlr(server->renderer);
    server->allocator = vela_gbm_allocator_create(vela_vulkan_render_fd(server->vulkan));
    if (!server->allocator) {
        wlr_log(WLR_ERROR, "Can't create the buffer allocator (GBM)");
        return false;
    }
    wlr_renderer_init_wl_shm(server->wlr_renderer, display);
    // GPU buffers shared with apps, without copies: the formats are those our
    // device can read.
    struct wlr_linux_dmabuf_v1 *dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(display, 4, server->wlr_renderer);
    // Explicit sync (§7.3): apps say when the buffer is ready and we say when
    // we're done reading it, with kernel timelines instead of the dmabufs'
    // implicit fences. Vulkan (Mesa, NVIDIA) and games use it.
    // WLR_RENDER_NO_EXPLICIT_SYNC=1 turns it off, as in the rest of wlroots.
    if (server->wlr_renderer->features.timeline && !vela_env_flag("WLR_RENDER_NO_EXPLICIT_SYNC")
        && wlr_linux_drm_syncobj_manager_v1_create(display, 1, vela_vulkan_render_fd(server->vulkan))) {
        wlr_log(WLR_INFO, "Explicit sync with apps (linux-drm-syncobj-v1) enabled");
    }

    // Basic protocols almost every modern application expects. With the
    // renderer, wlroots uploads app buffers to our textures on every commit
    // (only the changed part).
    server->compositor = wlr_compositor_create(display, 6, server->wlr_renderer);
    vela_ready_init(&server->ready, server->compositor);
    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);
    wlr_primary_selection_v1_device_manager_create(display);
    wlr_data_control_manager_v1_create(display);
    // The clipboard for those without a window: the shell's clipboard history
    // (Win+V) and the Snipping Tool.
    wlr_ext_data_control_manager_v1_create(display, 1);
    wlr_viewporter_create(display);
    wlr_single_pixel_buffer_manager_v1_create(display);
    wlr_fractional_scale_manager_v1_create(display, 1);
    wlr_screencopy_manager_v1_create(display);
    wlr_presentation_create(display, server->backend, 2);

    server->output_layout = wlr_output_layout_create(display);
    wlr_xdg_output_manager_v1_create(display, server->output_layout);
    vela_output_manager_init(server);
    vela_listen(&server->output_layout->events.change, &server->layout_change, handle_layout_change);

    server->scene = vela_scene_create();
    vela_scene_watch(server->scene, server->compositor);
    server->scene->linux_dmabuf = dmabuf;
    server->scene->event_loop = server->loop;

    // Creation order is stacking order (from the bottom).
    struct vela_tree *root = server->scene->root;
    struct vela_layers *layers = &server->layers;
    layers->background = vela_tree_create(root);
    layers->bottom = vela_tree_create(root);
    layers->windows = vela_tree_create(root);
    layers->top = vela_tree_create(root);
    layers->fullscreen = vela_tree_create(root);
    layers->top_above_fullscreen = vela_tree_create(root);
    layers->x11_popups = vela_tree_create(root);
    layers->overlay = vela_tree_create(root);
    // Above the windows and below the panels: the desktop being left.
    layers->windows_out = vela_tree_create(root);
    vela_node_place_above(&layers->windows_out->node, &layers->windows->node);
    layers->windows_out->node.ignores_input = true;
    layers->drag = vela_tree_create(root);
    layers->drag->node.ignores_input = true;
    layers->lock = vela_tree_create(root);

    vela_listen(&server->backend->events.new_output, &server->new_output, handle_new_output);

    // Windows: xdg-shell, xdg-decoration, foreign-toplevel, capture,
    // activation (view.c).
    vela_views_init(server);
    // The blur behind panels and apps that ask for it (§8.3).
    vela_background_effects_init(display, server->scene);
    vela_layers_init(server); // the shell pieces (layer.c)
    // Seat, cursor, mice, keyboards, gestures (input.c).
    server->input = vela_input_create(server);

    if (!wl_event_loop_add_signal(server->loop, SIGUSR1, handle_redraw_all, server)) {
        wlr_log_errno(WLR_ERROR, "Cannot register SIGUSR1 handler");
        return false;
    }
    vela_xwayland_init(server); // X11 apps (xwayland.c)
    server->lock = vela_lock_create(server); // lock and inactivity (lock.c)
    // Accessibility and screen color (a11y.c); VRR and tearing.
    server->scene->tearing_control = wlr_tearing_control_manager_v1_create(display, 1);
    server->a11y = vela_a11y_create(server);
    vela_output_load_settings(server);
    vela_a11y_load(server->a11y);
    server->workspaces = vela_workspaces_create(server); // virtual desktops (workspace.c)
    if (vela_env_one("VELA_DEBUG_ANIMATION")) {
        vela_motion_logger = log_animation;
    }

    if (!wl_event_loop_add_signal(server->loop, SIGINT, handle_terminate, display)) {
        wlr_log_errno(WLR_ERROR, "Cannot register SIGINT handler");
        return false;
    }
    if (!wl_event_loop_add_signal(server->loop, SIGTERM, handle_terminate, display)) {
        wlr_log_errno(WLR_ERROR, "Cannot register SIGTERM handler");
        return false;
    }
    make_realtime();
    return true;
}

struct vela_server *vela_server_create(bool *unsupported_gpu)
{
    if (unsupported_gpu) *unsupported_gpu = false;
    struct vela_server *server = calloc(1, sizeof(*server));
    server->runtime_dir_fd = -1;
    if (!init(server)) {
        // What was already created is left to the process, which is about to
        // end.
        if (unsupported_gpu) *unsupported_gpu = server->unsupported_gpu;
        free(server);
        return NULL;
    }
    return server;
}

bool vela_server_start(struct vela_server *server, const char *startup_command)
{
    // In the supervised session the supervisor holds the socket and hands it
    // to every compositor it starts (supervisor.c).
    const char *socket = NULL;
    char given[64] = "";
    const char *socket_fd = getenv("VELA_WAYLAND_SOCKET_FD");
    const char *socket_display = getenv("VELA_WAYLAND_DISPLAY");
    const char *runtime_fd_text = getenv("VELA_RUNTIME_DIR_FD");
    bool supervised = socket_fd && socket_display;
    if (supervised) {
        char *end = NULL;
        errno = 0;
        long inherited = runtime_fd_text ? strtol(runtime_fd_text, &end, 10) : -1;
        if (!runtime_fd_text || !*runtime_fd_text || !end || *end
            || errno != 0 || inherited < 0 || inherited > INT_MAX
            || !vela_runtime_dir_fd_private((int)inherited)
            || fcntl((int)inherited, F_SETFD, FD_CLOEXEC) != 0) {
            wlr_log(WLR_ERROR, "Supervisor did not provide a valid runtime-directory descriptor");
            unsetenv("VELA_RUNTIME_DIR_FD");
            unsetenv("VELA_WAYLAND_SOCKET_FD");
            unsetenv("VELA_WAYLAND_DISPLAY");
            return false;
        }
        server->runtime_dir_fd = (int)inherited;
        snprintf(given, sizeof(given), "%s", socket_display);
        if (wl_display_add_socket_fd(server->display, atoi(socket_fd)) == 0) {
            socket = given;
        }
        unsetenv("VELA_RUNTIME_DIR_FD");
        unsetenv("VELA_WAYLAND_SOCKET_FD");
        unsetenv("VELA_WAYLAND_DISPLAY");
    } else {
        unsetenv("VELA_RUNTIME_DIR_FD");
        socket = wl_display_add_socket_auto(server->display);
    }
    if (!socket) {
        wlr_log(WLR_ERROR, "Can't create the Wayland socket");
        return false;
    }
    snprintf(server->socket_name, sizeof(server->socket_name), "%s", socket);

    if (!wlr_backend_start(server->backend)) {
        wlr_log(WLR_ERROR, "Can't start the backend");
        return false;
    }

    setenv("WAYLAND_DISPLAY", socket, 1);
    // Only in the real session (from SDDM or a console): nested or headless,
    // Vela stays a guest of the environment it finds.
    if (server->session) {
        vela_session_set_environment();
    }
    // X11 apps launched from here go to our Xwayland, not to the one of the
    // host session (KDE) we may have started from.
#if WLR_HAS_XWAYLAND
    if (server->xwayland) {
        setenv("DISPLAY", server->xwayland->display_name, 1);
    } else {
        unsetenv("DISPLAY");
    }
#else
    unsetenv("DISPLAY");
#endif
    // Qt apps reconnect to the new compositor if this one crashes: only when
    // the supervisor restarts it on the same socket. Without one (nested,
    // headless) it does harm: when Vela closes, Qt apps try to reconnect and
    // crash inside Qt.
    if (supervised) {
        setenv("QT_WAYLAND_RECONNECT", "1", 1);
    } else {
        unsetenv("QT_WAYLAND_RECONNECT");
    }

    vela_commands_listen(server);
    if (server->session) {
        vela_session_run_hook("start");
    }

    wlr_log(WLR_INFO, "Vela running on WAYLAND_DISPLAY=%s%s", socket,
        getenv("VELA_RESTARTED") ? " (restarted after a crash)" : "");
    unsetenv("VELA_RESTARTED");
    // It was locked when the previous compositor went down: start locked,
    // black screen until vela-lock shows up.
    if (getenv("VELA_START_LOCKED")) {
        unsetenv("VELA_START_LOCKED");
        wlr_log(WLR_INFO, "The screen was locked: restarting locked");
        vela_lock_engage(server->lock);
        vela_lock_screen(server->lock);
    }
    if (server->nested) {
        wlr_log(WLR_INFO, "Nested mode: Alt shortcuts enabled");
    }
    if (startup_command && *startup_command) {
        vela_shell_start(server, startup_command);
    }
    vela_polkit_agent_start(server);
    return true;
}

void vela_server_run(struct vela_server *server)
{
    wl_display_run(server->display);
}

void vela_server_destroy(struct vela_server *server)
{
    // We are shutting down: the shell and the agent going away must not be
    // relaunched.
    vela_shell_stop(server);
    vela_child_stop(&server->polkit_agent);
    vela_commands_stop(server);
    if (server->session) {
        vela_session_run_hook("stop");
    }
    vela_xwayland_finish(server); // X11 windows before the Wayland clients

    // Closing the clients destroys windows and shell surfaces, which remove
    // themselves from our lists.
    wl_display_destroy_clients(server->display);

    // wlroots checks that no listener stays attached to the objects it
    // destroys: all the global ones are detached first.
    vela_output_manager_finish(server);
    wl_list_remove(&server->layout_change.link);
    wl_list_remove(&server->new_output.link);
    wl_list_remove(&server->new_layer_surface.link);
    vela_views_finish(server);
    vela_workspaces_destroy(server->workspaces);
    server->workspaces = NULL;
    vela_snapshots_finish(server);
    vela_snapping_destroy(server->snapping);
    server->snapping = NULL;
    vela_switcher_destroy(server->switcher);
    server->switcher = NULL;
    vela_interaction_destroy(server->interaction);
    server->interaction = NULL;
    vela_input_destroy(server->input);
    server->input = NULL;
    vela_a11y_destroy(server->a11y);
    server->a11y = NULL;

    wlr_xcursor_manager_destroy(server->cursor_manager);
    wlr_cursor_destroy(server->cursor);
    wlr_backend_destroy(server->backend); // destroys outputs and keyboards
    // After the outputs, which use them: the lock, the layers and the scene
    // (every node destroyed tells its scene).
    vela_lock_destroy(server->lock);
    server->lock = NULL;
    struct vela_layers *l = &server->layers;
    struct vela_tree *trees[] = {
        l->background, l->bottom, l->windows, l->windows_out, l->top, l->fullscreen, l->top_above_fullscreen,
        l->x11_popups, l->overlay, l->drag, l->lock,
    };
    for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); ++i) {
        vela_node_destroy(&trees[i]->node);
    }
    *l = (struct vela_layers) { 0 };
    vela_text_destroy(server->text);
    vela_icons_destroy(server->icons);
    vela_scene_destroy(server->scene);
    server->scene = NULL;
    vela_ready_finish(&server->ready);
    // Renderer, allocator and Vulkan device must be released after outputs,
    // scene and buffer listeners are gone. An earlier workaround skipped this
    // on normal exit after a nested amdgpu crash; the dangling buffer-listener
    // cause was since fixed by pairing buffers with wlr_buffer_finish().
    //
    // Keep an emergency hardware-specific opt-out while the driver matrix is
    // validated: no GPU resources are freed manually when that flag is set,
    // and the kernel reclaims them on process exit (the previous behavior).
    if (!vela_env_one("VELA_SKIP_VULKAN_CLEANUP")) {
        wlr_log(WLR_INFO, "Shutdown: releasing Vulkan renderer, allocator and device");
        // The renderer notifies its users (wlr_compositor) before teardown.
        wlr_renderer_destroy(server->wlr_renderer);
        wlr_allocator_destroy(server->allocator);
        vela_vulkan_destroy(server->vulkan); // last: everything else uses its device
    } else {
        wlr_log(WLR_ERROR, "Shutdown: skipping Vulkan resource cleanup (diagnostic override)");
    }
    wl_display_destroy(server->display);
    if (server->runtime_dir_fd >= 0) {
        close(server->runtime_dir_fd);
        server->runtime_dir_fd = -1;
    }
    free(server);
}

// ------------------------------------------------------- from the outputs --

void vela_server_output_destroyed(struct vela_server *server, struct vela_output *output)
{
    // The shell surfaces tied to this output must be closed.
    vela_layers_close_output(server, output);
    // At shutdown the backend destroys the outputs after snapping and
    // accessibility are gone.
    if (server->snapping) {
        vela_snap_end_zone(server, false); // the preview might be on this output
    }
    if (server->a11y) {
        vela_a11y_output_destroyed(server->a11y, output);
    }
    // A lock in progress must no longer wait for black on this output.
    if (server->lock) {
        vela_lock_output_rendered(server->lock, output);
    }
}

void vela_server_output_rendered(struct vela_server *server, struct vela_output *output)
{
    if (server->lock) {
        vela_lock_output_rendered(server->lock, output);
    }
}

void vela_server_animate(struct vela_server *server, int64_t present_ns)
{
    vela_xwayland_sync(server);
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        vela_view_update_shape(view); // corners and shadow for the current state
    }
    // What this frame shows (docs/renderer.md §4.2), and what can be retired:
    // no frame still to come, on any output, is presented before the wall
    // clock (docs/visual-coherence.md §6.2).
    double now_ms = present_ns / 1e6;
    double wall_ms = vela_now_ns() / 1e6;
    server->ticking = true;
    bool running = vela_a11y_tick(server->a11y, now_ms, wall_ms);
    running |= vela_views_tick(server, now_ms, wall_ms);
    running |= vela_snapshots_tick(server, now_ms, wall_ms);
    running |= vela_snap_tick_preview(server, now_ms);
    running |= vela_workspaces_tick(server, now_ms, wall_ms);
    if (running) {
        vela_server_schedule_frames(server);
    }
    server->ticking = false;
}
