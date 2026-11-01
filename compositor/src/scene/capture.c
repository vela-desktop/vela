// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scene/capture.h"

#include "render/renderer.h"
#include "scene/frame.h"
#include "scene/scene.h"
#include "view.h"

#include <drm_fourcc.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>

struct vela_window_capture {
    struct wlr_ext_image_capture_source_v1 base; // first member: reached with a cast
    struct vela_scene *scene;
    struct vela_node *node;
    struct vela_view *view;
    struct vela_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_swapchain *swapchain;
    struct vela_elements elements; // reused from one capture to the next
};

// The "frame" event carries the buffer just drawn.
struct frame_event {
    struct wlr_ext_image_capture_source_v1_frame_event base;
    struct wlr_buffer *buffer;
    struct vela_renderer *renderer;
    struct timespec when;
};

// A deferred protocol frame owns its renderer request until ready/failure or
// protocol destruction. Repeated source damage while it runs must not queue a
// second copy of the same frame.
struct pending_capture {
    struct wlr_ext_image_copy_capture_frame_v1 *frame;
    struct vela_readback *readback;
    enum wl_output_transform transform;
    struct timespec when;
    struct wl_listener destroy;
    struct wl_listener constraints;
};

static void destroy_pending(struct pending_capture *pending)
{
    wl_list_remove(&pending->destroy.link);
    wl_list_remove(&pending->constraints.link);
    vela_readback_cancel(pending->readback);
    free(pending);
}

static void handle_frame_destroy(struct wl_listener *listener, void *data)
{
    struct pending_capture *pending = wl_container_of(listener, pending, destroy);
    destroy_pending(pending);
}

static void handle_constraints(struct wl_listener *listener, void *data)
{
    struct pending_capture *pending = wl_container_of(listener, pending, constraints);
    wlr_ext_image_copy_capture_frame_v1_fail(pending->frame,
        EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
}

static void readback_done(void *data, bool ok)
{
    struct pending_capture *pending = data;
    struct wlr_ext_image_copy_capture_frame_v1 *frame = pending->frame;
    enum wl_output_transform transform = pending->transform;
    struct timespec when = pending->when;
    // The renderer already retired the request. Detach before ready/fail,
    // which synchronously destroys the protocol frame and emits destroy.
    pending->readback = NULL;
    destroy_pending(pending);
    if (ok) {
        wlr_ext_image_copy_capture_frame_v1_ready(frame, transform, &when);
    } else {
        wlr_ext_image_copy_capture_frame_v1_fail(frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
    }
}

void vela_capture_copy_frame(struct wlr_ext_image_capture_source_v1 *source,
    struct wlr_ext_image_copy_capture_frame_v1 *frame, struct wlr_buffer *buffer,
    struct vela_renderer *renderer, struct wl_event_loop *loop,
    enum wl_output_transform transform, const struct timespec *when)
{
    if (wl_signal_get(&frame->events.destroy, handle_frame_destroy)) {
        return;
    }
    if (!buffer) {
        wlr_ext_image_copy_capture_frame_v1_fail(frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED);
        return;
    }
    if (buffer->width != frame->buffer->width || buffer->height != frame->buffer->height) {
        wlr_ext_image_copy_capture_frame_v1_fail(frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
        return;
    }
    struct wlr_dmabuf_attributes dmabuf;
    if (wlr_buffer_get_dmabuf(frame->buffer, &dmabuf)) {
        // wlroots' GPU-to-GPU copy path has its own synchronization contract.
        if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, buffer, vela_renderer_wlr(renderer))) {
            wlr_ext_image_copy_capture_frame_v1_ready(frame, transform, when);
        }
        return;
    }
    struct pending_capture *pending = calloc(1, sizeof(*pending));
    if (!pending) {
        wlr_ext_image_copy_capture_frame_v1_fail(frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }
    pending->frame = frame;
    pending->transform = transform;
    pending->when = *when;
    pending->readback = vela_renderer_readback(renderer, loop, buffer, frame->buffer, readback_done, pending);
    if (!pending->readback) {
        free(pending);
        wlr_ext_image_copy_capture_frame_v1_fail(frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
        return;
    }
    pending->destroy.notify = handle_frame_destroy;
    wl_signal_add(&frame->events.destroy, &pending->destroy);
    pending->constraints.notify = handle_constraints;
    wl_signal_add(&source->events.constraints_update, &pending->constraints);
}

static void copy_frame(struct wlr_ext_image_capture_source_v1 *source, struct wlr_ext_image_copy_capture_frame_v1 *frame,
    struct wlr_ext_image_capture_source_v1_frame_event *base_event)
{
    struct vela_window_capture *capture = (struct vela_window_capture *)source;
    struct frame_event *event = (struct frame_event *)base_event;
    vela_capture_copy_frame(source, frame, event->buffer, event->renderer, capture->scene->event_loop,
        WL_OUTPUT_TRANSFORM_NORMAL, &event->when);
}

// A buffer as large as the window, in a format we can both draw and read back
// (for shared-memory captures).
static bool update_constraints(struct vela_window_capture *capture)
{
    struct wlr_box box = vela_view_frame_box(capture->view);
    if (box.width <= 0 || box.height <= 0) {
        return false;
    }
    if (capture->swapchain && capture->swapchain->width == box.width && capture->swapchain->height == box.height) {
        return true;
    }
    const struct wlr_drm_format *renderable
        = wlr_drm_format_set_get(vela_renderer_render_formats(capture->renderer), DRM_FORMAT_ARGB8888);
    if (!renderable) {
        return false;
    }
    const struct wlr_drm_format_set *readable = vela_renderer_texture_formats(capture->renderer);
    struct wlr_drm_format_set both = { 0 };
    for (size_t i = 0; i < renderable->len; ++i) {
        if (wlr_drm_format_set_has(readable, DRM_FORMAT_ARGB8888, renderable->modifiers[i])) {
            wlr_drm_format_set_add(&both, DRM_FORMAT_ARGB8888, renderable->modifiers[i]);
        }
    }
    wlr_swapchain_destroy(capture->swapchain);
    capture->swapchain = NULL;
    const struct wlr_drm_format *format = wlr_drm_format_set_get(&both, DRM_FORMAT_ARGB8888);
    if (format) {
        capture->swapchain = wlr_swapchain_create(capture->allocator, box.width, box.height, format);
    }
    wlr_drm_format_set_finish(&both);
    if (!capture->swapchain) {
        return false;
    }
    wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&capture->base, capture->swapchain,
        vela_renderer_wlr(capture->renderer));
    wl_signal_emit_mutable(&capture->base.events.constraints_update, NULL);
    return true;
}

// Draws the window and offers it to whoever waits for a frame.
static void request_frame(struct wlr_ext_image_capture_source_v1 *source, bool schedule_frame)
{
    struct vela_window_capture *capture = (struct vela_window_capture *)source;
    if (!update_constraints(capture)) {
        return;
    }
    struct wlr_box box = vela_view_frame_box(capture->view);
    struct wlr_buffer *buffer = wlr_swapchain_acquire(capture->swapchain);
    if (!buffer) {
        return;
    }
    bool ok = false;
    struct vela_pass *pass = vela_renderer_begin_pass(capture->renderer, buffer);
    if (pass) {
        const struct wlr_box whole = { 0, 0, buffer->width, buffer->height };
        const struct wlr_render_color transparent = { 0.0f, 0.0f, 0.0f, 0.0f };
        vela_pass_add_rect(pass, &whole, &transparent, NULL, false, NULL, 0.0f);
        const struct vela_build_params params = {
            .origin_x = box.x,
            .origin_y = box.y,
            .scale = 1.0,
            .bounds = whole,
            .capture_root = true,
        };
        vela_build_elements(capture->scene, capture->node, &params, &capture->elements);
        for (int i = 0; i < capture->elements.count; ++i) {
            capture->elements.items[i].visible = true;
        }
        vela_draw_elements(capture->scene, pass, &capture->elements, NULL, WL_OUTPUT_TRANSFORM_NORMAL, whole.width,
            whole.height);
        struct vela_pass_result result;
        ok = vela_pass_submit(pass, &result);
        if (ok) {
            vela_add_release_points(capture->scene, &capture->elements, capture->renderer, result.sync_point);
        }
    }
    if (ok) {
        pixman_region32_t damage;
        pixman_region32_init_rect(&damage, 0, 0, (unsigned)buffer->width, (unsigned)buffer->height);
        struct frame_event event = { .base = { .damage = &damage }, .buffer = buffer, .renderer = capture->renderer };
        clock_gettime(CLOCK_MONOTONIC, &event.when);
        wl_signal_emit_mutable(&capture->base.events.frame, &event.base);
        pixman_region32_fini(&damage);
    }
    wlr_buffer_unlock(buffer);
}

static const struct wlr_ext_image_capture_source_v1_interface capture_impl = {
    .request_frame = request_frame,
    .copy_frame = copy_frame,
};

struct vela_window_capture *vela_window_capture_create(struct vela_scene *scene, struct vela_node *source,
    struct vela_view *view, struct vela_renderer *renderer, struct wlr_allocator *allocator)
{
    struct vela_window_capture *capture = calloc(1, sizeof(*capture));
    wlr_ext_image_capture_source_v1_init(&capture->base, &capture_impl);
    capture->scene = scene;
    capture->node = source;
    capture->view = view;
    capture->renderer = renderer;
    capture->allocator = allocator;
    return capture;
}

void vela_window_capture_destroy(struct vela_window_capture *capture)
{
    if (!capture) {
        return;
    }
    wlr_ext_image_capture_source_v1_finish(&capture->base);
    wlr_swapchain_destroy(capture->swapchain);
    free(capture->elements.items);
    free(capture);
}

struct wlr_ext_image_capture_source_v1 *vela_window_capture_source(struct vela_window_capture *capture)
{
    update_constraints(capture);
    return &capture->base;
}
