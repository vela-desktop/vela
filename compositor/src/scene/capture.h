// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SCENE_CAPTURE_H
#define VELA_SCENE_CAPTURE_H

// Capturing a window's image (Alt+Tab previews, and later sharing a window):
// our renderer draws it alone, without what lies above or below it, even when
// covered or minimized. Owned by the window, which destroys it with itself.

#include <stdbool.h>
#include <time.h>
#include <wayland-server-protocol.h>

struct wl_event_loop;
struct wlr_buffer;
struct wlr_ext_image_copy_capture_frame_v1;
struct vela_server;
struct vela_node;
struct vela_renderer;
struct vela_scene;
struct vela_view;
struct vela_window_capture;
struct wlr_allocator;
struct wlr_ext_image_capture_source_v1;

// `source`: the window's tree; `view` gives its frame.
struct vela_window_capture *vela_window_capture_create(struct vela_scene *scene, struct vela_node *source,
    struct vela_view *view, struct vela_renderer *renderer, struct wlr_allocator *allocator);
void vela_window_capture_destroy(struct vela_window_capture *capture);

// The source, with sizes and formats kept up to date with the window.
struct wlr_ext_image_capture_source_v1 *vela_window_capture_source(struct vela_window_capture *capture);

// Vela owns the output source so SHM completion can be deferred. Protocol
// session/frame objects, DMA-BUF copies, and cursor metadata remain wlroots'.
bool vela_output_capture_manager_create(struct vela_server *server);
void vela_capture_copy_frame(struct wlr_ext_image_capture_source_v1 *source,
    struct wlr_ext_image_copy_capture_frame_v1 *frame, struct wlr_buffer *buffer,
    struct vela_renderer *renderer, struct wl_event_loop *loop,
    enum wl_output_transform transform, const struct timespec *when);

#endif
