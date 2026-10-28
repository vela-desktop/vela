// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_RENDER_RENDERER_H
#define VELA_RENDER_RENDERER_H

// Vela's renderer (docs/renderer.md §6-7): everything drawn goes through
// here, on a Vulkan 1.4 device of our own.
//
// To wlroots it presents itself as a wlr_renderer. So what wlroots draws on
// its own uses our pixels and our device too: the hardware cursor, output
// captures (screencopy, ext-image-copy-capture), uploads of app buffers.
// There is no second renderer.
//
// Who owns what:
// - vela_vulkan (device, formats) is created by the server and destroyed
//   last, after the renderer;
// - vela_renderer belongs to wlroots: born with vela_renderer_create, it
//   dies with wlr_renderer_destroy(vela_renderer_wlr(r));
// - a vela_pass lives from vela_renderer_begin_pass to vela_pass_submit,
//   which consumes it;
// - textures are created and destroyed by wlroots (wlr_texture_from_buffer,
//   wlr_texture_destroy); their Vulkan resources go away when the GPU is
//   done with them.

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-protocol.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/box.h>

struct vela_vulkan;
struct vela_renderer;
struct vela_pass;
struct wlr_allocator;
struct wlr_buffer;
struct wlr_drm_format_set;
struct wlr_drm_syncobj_timeline;
struct wlr_texture;

// ------------------------------------------------------------- device --

// backend_drm_fd: the backend's DRM device (-1 if it has none, such as
// headless): the matching GPU is chosen. NULL if Vulkan 1.4 or the required
// extensions are missing; the reason is already in the log.
struct vela_vulkan *vela_vulkan_create(int backend_drm_fd);
void vela_vulkan_destroy(struct vela_vulkan *vk);
// The render node of the same GPU, opened by the device: the allocator and the
// syncobj timelines need it.
int vela_vulkan_render_fd(const struct vela_vulkan *vk);

// Allocator of output buffers (docs/renderer.md §7.2): GBM on the render node,
// buffers exported as dmabufs with an explicit modifier, fit for both Vulkan
// drawing and scanout. Destroyed with wlr_allocator_destroy(). Doesn't close
// render_fd.
struct wlr_allocator *vela_gbm_allocator_create(int render_fd);

// Separate allocator for the optional cursor-only swapchain on DRM outputs.
// It never allocates scanout/capture buffers and refuses non-renderable BOs.
struct wlr_allocator *vela_gbm_cursor_allocator_create(int render_fd, struct vela_renderer *renderer);

// Check the *actual* Vulkan DMA-BUF import/color attachment, not only the
// GBM flags. A successful probe caches the target for later render passes.
bool vela_renderer_validate_target(struct vela_renderer *renderer, struct wlr_buffer *buffer);

// ----------------------------------------------------------- renderer --

struct vela_renderer *vela_renderer_create(struct vela_vulkan *vk);
struct wlr_renderer *vela_renderer_wlr(struct vela_renderer *renderer);
int vela_renderer_render_fd(const struct vela_renderer *renderer);

// The dmabuf formats (and modifiers) that can be drawn on, and those that can
// be read as textures.
const struct wlr_drm_format_set *vela_renderer_render_formats(const struct vela_renderer *renderer);
const struct wlr_drm_format_set *vela_renderer_texture_formats(const struct vela_renderer *renderer);

// The renderer's syncobj timeline (linux-drm-syncobj-v1, §7.3): every drawing
// signals a point of it when the GPU has finished. These are the release
// points of app buffers. NULL if the kernel doesn't support it.
struct wlr_drm_syncobj_timeline *vela_renderer_sync_timeline(const struct vela_renderer *renderer);

// The last point of the Vulkan timeline the GPU has finished.
uint64_t vela_renderer_completed(struct vela_renderer *renderer);

// When the GPU started and finished a drawing (§4.3). With calibrated
// timestamps the times are on CLOCK_MONOTONIC (absolute); otherwise only the
// duration counts (end_ns).
struct vela_gpu_timing {
    int64_t start_ns;
    int64_t end_ns;
    bool absolute;
};
// true if the drawing (slot, point) has finished and the measurement is still
// there.
bool vela_renderer_read_timing(struct vela_renderer *renderer, int slot, uint64_t point, struct vela_gpu_timing *out);

// The texture is ours and its format has no alpha (an opaque buffer).
bool vela_texture_is_opaque(struct wlr_texture *texture);

// Deferred SHM capture from buffers produced on this renderer's queue. Sources
// must be compositor-owned render targets, never directly scanned-out clients.
// Up to four requests / 128 MiB are retained. NULL means not queued; otherwise
// done runs once on the Wayland loop after full pixels, or failure. Cancellation
// releases ownership without calling done. All functions run on that same loop.
struct vela_readback;
struct wl_event_loop;
struct vela_readback *vela_renderer_readback(struct vela_renderer *renderer, struct wl_event_loop *loop,
    struct wlr_buffer *src, struct wlr_buffer *dst, void (*done)(void *, bool), void *data);
void vela_readback_cancel(struct vela_readback *readback);
void vela_renderer_cancel_readbacks(struct vela_renderer *renderer);
struct vela_readback_stats {
    size_t pending, bytes;
    uint64_t completed, cancelled, failed;
};
struct vela_readback_stats vela_renderer_readback_stats(const struct vela_renderer *renderer);

// ------------------------------------------------------------- drawing --

// Starts drawing on `buffer` (a dmabuf in one of the drawing formats). NULL if
// it can't.
struct vela_pass *vela_renderer_begin_pass(struct vela_renderer *renderer, struct wlr_buffer *buffer);

struct wlr_render_pass *vela_pass_wlr(struct vela_pass *pass);

struct vela_texture_draw {
    struct wlr_texture *texture; // ours; others are ignored
    struct wlr_fbox src; // in texture pixels; empty: all of it
    struct wlr_box dst; // in target pixels
    enum wl_output_transform transform; // applied to the texture
    float alpha;
    bool linear; // bilinear filter (bicubic when magnifying); false: 1:1 copy
    bool blend;
    const pixman_region32_t *clip; // in target pixels; NULL: none
    // Explicit sync (linux-drm-syncobj-v1): the point to wait for before
    // reading the texture, instead of the dmabuf's implicit fence.
    struct wlr_drm_syncobj_timeline *wait_timeline;
    uint64_t wait_point;
    // Rounded clip (§8.1), in target pixels: radius 0 or an empty rectangle,
    // no clip.
    struct wlr_box shape_rect;
    float shape_radius;
};
void vela_pass_add_texture(struct vela_pass *pass, const struct vela_texture_draw *draw);

// Color as in wlroots: sRGB, premultiplied. shape_rect can be NULL.
void vela_pass_add_rect(struct vela_pass *pass, const struct wlr_box *box, const struct wlr_render_color *color,
    const pixman_region32_t *clip, bool blend, const struct wlr_box *shape_rect, float shape_radius);

// Shadow of a rounded rectangle (§8.2) inside `box`: cast by `caster`, not
// drawn under `window`. Premultiplied sRGB color.
void vela_pass_add_shadow(struct vela_pass *pass, const struct wlr_box *box, const struct wlr_box *caster,
    const struct wlr_box *window, float radius, float sigma, const struct wlr_render_color *color,
    const pixman_region32_t *clip);

// Live blur (§8.3) under a panel: reads what has already been drawn behind
// `region` (target pixels, clip included), blurs it (dual Kawase) and draws it
// dithered. The shape comes from the alpha of `panel`, the surface that goes
// on top (added afterwards, as usual). `strength`: the width of the passes, in
// pixels; `tint`: Vela's acrylic recipe with this tint (premultiplied sRGB),
// or NULL for the plain blur a client asked for. Nothing happens if the target
// can't be read.
void vela_pass_add_blur(struct vela_pass *pass, const struct vela_texture_draw *panel,
    const pixman_region32_t *region, float strength, const struct wlr_render_color *tint);

// How far around a zone the blur reads, in pixels.
int vela_blur_reach(float strength);

// The output's color filter (night light, color filters): a 3x3 matrix by
// rows, in linear space, applied to everything drawn. NULL: none.
void vela_pass_set_color_filter(struct vela_pass *pass, const float *matrix);

// Measures the GPU times of this drawing (vela_renderer_read_timing).
void vela_pass_measure(struct vela_pass *pass);

// When the work is done, also signals this point (for wlroots, such as a
// capture with explicit sync).
void vela_pass_signal_on_done(struct vela_pass *pass, struct wlr_drm_syncobj_timeline *timeline, uint64_t point);

// Submits the drawing and frees the pass. In the result: the renderer timeline
// point, the measurement slot (-1: not measured) and the syncobj timeline
// point signaled at the end (0 if none): the release of the app buffers this
// drawing read.
struct vela_pass_result {
    uint64_t point;
    int timing_slot;
    uint64_t sync_point;
};
bool vela_pass_submit(struct vela_pass *pass, struct vela_pass_result *result);

#endif
