// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SCENE_FRAME_H
#define VELA_SCENE_FRAME_H

// From the scene to an output's pixels (docs/renderer.md §6): the scene
// flattened into a list of quads, the covered parts dropped, the damage found
// by comparing with the previous frame, and only that redrawn. When a single
// opaque app covers the output, its buffer goes straight to the primary plane
// (direct scanout, §5.3).

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_damage_ring.h>

struct vela_node;
struct vela_output;
struct vela_pass;
struct vela_renderer;
struct vela_scene;
struct wlr_color_transform;
struct wlr_drm_syncobj_timeline;
struct wlr_output;
struct wlr_output_state;
struct wlr_surface;
struct wlr_texture;

// A quad to draw, in target pixels.
struct vela_element {
    const void *key; // who it is: the surface or our node
    struct wlr_surface *surface; // app surfaces, otherwise NULL
    struct wlr_texture *texture; // NULL: solid color
    struct wlr_render_color color;
    struct wlr_fbox src;
    enum wl_output_transform transform;
    struct wlr_box box;
    float opacity;
    bool linear; // bilinear filter; false: 1:1 copy
    // From the surface to pixels: pixel = origin + scale * logical coordinate.
    double origin_x, origin_y;
    double scale_x, scale_y;
    // The shape (§8.1): rounded clip in pixels; radius 0, none.
    struct wlr_box shape_rect;
    float shape_radius;
    // Shadow (§8.2) if sigma > 0: cast by shape_rect, not under shadow_window.
    struct wlr_box shadow_window;
    float shadow_sigma;
    // Blur behind the surface (ext-background-effect, §8.3): where, in pixels
    // (empty: none).
    struct wlr_box blur_box;
    bool visible; // not entirely covered
    int64_t visible_area; // uncovered pixels
    int order; // position in the list, from the bottom
};

// A list of elements, reused from frame to frame (it only grows).
struct vela_elements {
    struct vela_element *items;
    int count, capacity;
};

// Flattens `root` (bottom to top) into `out` (emptied first). The logical
// point (origin_x, origin_y) lands on pixel (0, 0); `bounds`: the target's
// pixels.
struct vela_build_params {
    double origin_x, origin_y;
    double scale;
    struct wlr_box bounds;
    // Captures: the root is drawn even when hidden (minimized window) and
    // without its opacity (animations).
    bool capture_root;
};
void vela_build_elements(struct vela_scene *scene, struct vela_node *root, const struct vela_build_params *params,
    struct vela_elements *out);

// Draws the visible elements inside `clip` (buffer pixels; NULL: all).
// Elements are in the rotated output space (width x height); the buffer can be
// rotated relative to it.
void vela_draw_elements(struct vela_scene *scene, struct vela_pass *pass, const struct vela_elements *elements,
    const pixman_region32_t *clip, enum wl_output_transform output_transform, int width, int height);

// After a drawing that read `elements`: apps with explicit sync get their
// buffers back when the renderer timeline reaches `sync_point` (the GPU has
// finished).
void vela_add_release_points(struct vela_scene *scene, const struct vela_elements *elements,
    struct vela_renderer *renderer, uint64_t sync_point);

// The last delivered frame: to measure its cost (§4.3) and, with the virtual
// vblank, to know when it's ready.
struct vela_frame_delivered {
    uint64_t point; // renderer timeline point; 0: no GPU drawing
    int timing_slot;
    bool scanout; // an app buffer straight on the output
    bool tearing; // and shown at once, without waiting for the vblank
};

// The output as the scene sees it. Created and destroyed by its vela_output,
// which it asks for frames.
struct vela_output_frame {
    struct wl_list link; // vela_scene.frames
    struct vela_scene *scene;
    struct vela_renderer *renderer;
    struct wlr_output *output;
    struct vela_output *owner;
    struct wlr_damage_ring ring; // buffer-age history in swapchain-buffer coordinates
    pixman_region32_t scene_damage; // current scene damage in upright output coordinates
    int width, height;
    float scale;
    enum wl_output_transform transform; // damage history is bound to this orientation

    // The previous frame and the one being built (they swap): the damage comes
    // from them.
    struct vela_elements last;
    struct vela_elements current;
    // Match flags for the previous frame; reused per output, only growing.
    bool *found;
    int found_capacity;
    // The surfaces visible in the last frame (for frame callbacks).
    struct wlr_surface **visible_surfaces;
    int visible_count, visible_capacity;

    struct vela_frame_delivered delivered;
    bool scanout; // the last frame was a direct scanout
    bool tearing; // the last scanout tore
    const char *scanout_reason; // why there is no scanout (VELA_DEBUG_SCANOUT)
    double zoom;
    double zoom_x, zoom_y;

    // Night light in the monitor's gamma: the version applied (the scene's),
    // if the monitor accepts it.
    struct wlr_color_transform *night_transform;
    bool night_commit_pending;
    bool night_gamma_committed; // last successful color commit installed a LUT
    bool night_clear_needed; // an old committed LUT still needs clearing
    uint32_t night_version;
    bool night_in_gamma; // use the gamma path instead of the Vulkan filter
    bool gamma_refused; // current fallback after a rejected gamma test/commit
    bool gamma_retry_pending; // one explicit retry, not every animation frame
    bool cursor_locked; // with the filter in drawing, we draw the cursor ourselves
    // Explicit sync for scanout: the backend signals here the release of an
    // app's buffer when it stops showing it.
    struct wlr_drm_syncobj_timeline *scanout_timeline;
    uint64_t scanout_point;
    int feedback_debounce;
    struct wlr_surface *feedback_surface; // who has our scanout feedback

    struct wl_listener damage;
    struct wl_listener needs_frame;
};

struct vela_output_frame *vela_output_frame_create(struct vela_scene *scene, struct vela_renderer *renderer,
    struct wlr_output *output, struct vela_output *owner);
void vela_output_frame_destroy(struct vela_output_frame *frame);

// Builds the frame for the output at (lx, ly) in the layout and commits it if
// there is something to show. false: nothing to do. `pending`: state to apply
// in the same commit (new mode or scale): the frame is already drawn at the
// new size, without the black buffer wlroots would otherwise put.
bool vela_output_frame_render(struct vela_output_frame *frame, double lx, double ly, struct wlr_output_state *pending);

// Someone (wlroots) wrote into the output's buffers: none of them holds what
// we think anymore.
void vela_output_frame_reset_damage(struct vela_output_frame *frame);
void vela_output_frame_damage_whole(struct vela_output_frame *frame);
// Color changes invalidate pixels only when the effect is drawn in Vulkan.
void vela_output_frame_color_changed(struct vela_output_frame *frame, bool filter_changed, bool night_changed);
// Reconsider a rejected DRM gamma LUT on reactivation, a mode change or the
// accessibility timer (at most once per minute), never on each color tween tick.
void vela_output_frame_retry_night_light(struct vela_output_frame *frame);

// After the frame: frame callbacks to the visible surfaces paced by this
// output.
void vela_output_frame_send_frame_done(struct vela_output_frame *frame, const struct timespec *when);

// Magnifier (Accessibility): the output shows the layout area starting at
// logical point (x, y), magnified `zoom` times. zoom 1: the output as it is.
void vela_output_frame_set_magnifier(struct vela_output_frame *frame, double zoom, double x, double y);

// From the scene: a surface shown by this output committed, or goes away.
bool vela_output_frame_shows(struct vela_output_frame *frame, struct wlr_surface *surface);
void vela_output_frame_surface_committed(struct vela_output_frame *frame, struct wlr_surface *surface);
void vela_output_frame_surface_destroyed(struct vela_output_frame *frame, struct wlr_surface *surface);

#endif
