// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scene/frame.h"
#include "scene/night_light_policy.h"

#include "geometry.h"
#include "listen.h"
#include "output.h"
#include "render/renderer.h"
#include "scene/effects.h"
#include "scene/scene.h"
#include "scene/surface.h"
#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/headless.h>
#include <wlr/render/color.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>

// ------------------------------------------------------------- helpers --

// Night light as the monitor's gamma table: each sRGB-encoded value goes back
// to linear light, takes its gain and is encoded again.
static struct wlr_color_transform *night_lut(const float gains[3])
{
    enum { SIZE = 256 };
    uint16_t channels[3][SIZE];
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < SIZE; ++i) {
            double x = (double)i / (double)(SIZE - 1);
            double linear = (x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4)) * gains[c];
            double encoded = linear <= 0.0031308 ? linear * 12.92 : 1.055 * pow(linear, 1.0 / 2.4) - 0.055;
            channels[c][i] = (uint16_t)lround(vela_clampd(encoded, 0.0, 1.0) * 65535.0);
        }
    }
    return wlr_color_transform_init_lut_3x1d(SIZE, channels[0], channels[1], channels[2]);
}

static bool same_box(const struct wlr_box *a, const struct wlr_box *b)
{
    return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}

static bool same_fbox(const struct wlr_fbox *a, const struct wlr_fbox *b)
{
    return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}

static bool same_color(const struct wlr_render_color *a, const struct wlr_render_color *b)
{
    return a->r == b->r && a->g == b->g && a->b == b->b && a->a == b->a;
}

static int64_t region_area(const pixman_region32_t *region)
{
    int count = 0;
    const pixman_box32_t *rects = pixman_region32_rectangles(region, &count);
    int64_t area = 0;
    for (int i = 0; i < count; ++i) {
        area += (int64_t)(rects[i].x2 - rects[i].x1) * (rects[i].y2 - rects[i].y1);
    }
    return area;
}

static void union_box(pixman_region32_t *region, const struct wlr_box *box)
{
    pixman_region32_union_rect(region, region, box->x, box->y, (unsigned)(box->width > 0 ? box->width : 0),
        (unsigned)(box->height > 0 ? box->height : 0));
}

static struct vela_element *push_element(struct vela_elements *list)
{
    list->items = vela_grow(list->items, &list->capacity, list->count + 1, sizeof(*list->items));
    struct vela_element *e = &list->items[list->count++];
    memset(e, 0, sizeof(*e));
    return e;
}

// A logical rectangle in pixels: the edges are rounded, not position and size
// (§3.2), so adjacent rectangles stay adjacent.
static struct wlr_box to_pixels(double x, double y, double width, double height, const struct vela_build_params *p)
{
    struct vela_pixel_box box = vela_edges_to_pixels(x, y, width, height, p->origin_x, p->origin_y, p->scale);
    return (struct wlr_box) { box.x, box.y, box.width, box.height };
}

static bool on_screen(const struct wlr_box *box, const struct vela_build_params *p)
{
    struct wlr_box clipped;
    return box->width > 0 && box->height > 0 && wlr_box_intersection(&clipped, box, &p->bounds);
}

// 1:1 copy (no resampling) when a buffer pixel lands exactly on an output
// pixel (§3.3).
static bool is_one_to_one(const struct wlr_fbox *src, const struct wlr_box *box, enum wl_output_transform transform)
{
    return vela_one_to_one(src->x, src->y, src->width, src->height, box->width, box->height,
        transform & WL_OUTPUT_TRANSFORM_90);
}

// The blur width in output pixels: the radius is in logical units, so the same
// blur at every scale (§8.3).
static float blur_strength(double scale)
{
    return vela_clampf((float)(1.25 * scale), 1.0f, 3.0f);
}

// A region of the surface (in its coordinates) in pixels, within the
// element. `outward` covers every pixel the region touches (blur); otherwise
// only the pixels it fills entirely (the opaque region must never cover too
// much). Clamped while still in floating point: clients send INT32_MAX for
// "the whole surface", which scaled would not fit an int.
static void surface_region_to_pixels(const struct vela_element *e, const pixman_region32_t *region, bool outward,
    pixman_region32_t *out)
{
    double left = e->box.x, top = e->box.y;
    double right = left + e->box.width, bottom = top + e->box.height;
    pixman_region32_clear(out);
    int count = 0;
    const pixman_box32_t *rects = pixman_region32_rectangles(region, &count);
    for (int i = 0; i < count; ++i) {
        double x1 = e->origin_x + rects[i].x1 * e->scale_x;
        double y1 = e->origin_y + rects[i].y1 * e->scale_y;
        double x2 = e->origin_x + rects[i].x2 * e->scale_x;
        double y2 = e->origin_y + rects[i].y2 * e->scale_y;
        int px1 = (int)fmax(left, outward ? floor(x1) : ceil(x1));
        int py1 = (int)fmax(top, outward ? floor(y1) : ceil(y1));
        int px2 = (int)fmin(right, outward ? ceil(x2) : floor(x2));
        int py2 = (int)fmin(bottom, outward ? ceil(y2) : floor(y2));
        if (px2 > px1 && py2 > py1) {
            pixman_region32_union_rect(out, out, px1, py1, (unsigned)(px2 - px1), (unsigned)(py2 - py1));
        }
    }
}

// ----------------------------------------------------------- flattening --

// The rounded clip inherited from an ancestor's shape (§8.1).
struct clip {
    struct wlr_box rect;
    float radius;
};

static void apply_clip(struct vela_element *e, const struct clip *clip)
{
    if (clip->radius > 0.0f) {
        e->shape_rect = clip->rect;
        e->shape_radius = clip->radius;
    }
}

// A shape's shadows, like Windows 11: a wide soft one plus a narrow "contact"
// one, stronger for the active window (§8.2).
static void add_shadows(const struct vela_tree *tree, const struct wlr_box *window, float radius, float opacity,
    bool active, const struct vela_build_params *p, struct vela_elements *out)
{
    static const struct {
        double offset_y; // logical
        double sigma; // logical
        float active_alpha;
        float inactive_alpha;
    } layers[] = {
        { 8.0, 14.0, 0.42f, 0.24f },
        { 1.0, 2.0, 0.30f, 0.16f },
    };
    for (size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); ++i) {
        float sigma = (float)(layers[i].sigma * p->scale);
        struct wlr_box caster = *window;
        caster.y += (int)lround(layers[i].offset_y * p->scale);
        int reach = (int)ceilf(3.0f * sigma) + 1;
        struct wlr_box box = { caster.x - reach, caster.y - reach, caster.width + 2 * reach, caster.height + 2 * reach };
        if (!on_screen(&box, p)) {
            continue;
        }
        float alpha = (active ? layers[i].active_alpha : layers[i].inactive_alpha) * opacity;
        struct vela_element *e = push_element(out);
        // A name per layer, inside the tree itself (no other node or surface
        // can have that address).
        e->key = (const char *)tree + 1 + i;
        e->color = (struct wlr_render_color) { 0.0f, 0.0f, 0.0f, alpha };
        e->box = box;
        e->opacity = 1.0f;
        e->shape_rect = caster;
        e->shape_radius = radius;
        e->shadow_window = *window;
        e->shadow_sigma = sigma;
    }
}

// What visit() needs for the surfaces of a surface node.
struct visit {
    struct vela_scene *scene;
    const struct vela_build_params *params;
    struct vela_elements *out;
    double lx, ly;
    float opacity;
    struct clip clip;
};

// Sharpness (§3.3, §3.4): an image drawn at the output's scale (a buffer as
// large as its physical area, up to rounding) snaps to a physical pixel and
// covers exactly the buffer's pixels: a 1:1 copy, unfiltered, even when the
// logical position falls in the middle of a pixel. Whoever drew it: an app, or
// the compositor (title, icon, glyphs).
static void snap_to_buffer(struct wlr_box *box, const struct wlr_fbox *src, enum wl_output_transform transform,
    double width, double height, double scale)
{
    bool swapped = transform & WL_OUTPUT_TRANSFORM_90;
    double buffer_width = swapped ? src->height : src->width;
    double buffer_height = swapped ? src->width : src->height;
    if (vela_buffer_matches_area(src->x, src->y, buffer_width, buffer_height, width, height, scale)) {
        box->width = (int)buffer_width;
        box->height = (int)buffer_height;
    }
}

static void add_surface(struct wlr_surface *surface, int sx, int sy, void *data)
{
    struct visit *v = data;
    const struct vela_build_params *p = v->params;
    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    if (!texture) {
        return; // no buffer: nothing to show
    }
    double lx = v->lx + sx;
    double ly = v->ly + sy;
    double width = surface->current.width;
    double height = surface->current.height;
    struct wlr_fbox src;
    wlr_surface_get_buffer_source_box(surface, &src);
    enum wl_output_transform transform = wlr_output_transform_invert(surface->current.transform);
    struct wlr_box box = to_pixels(lx, ly, width, height, p);

    snap_to_buffer(&box, &src, transform, width, height, p->scale);
    if (!on_screen(&box, p)) {
        return;
    }
    struct vela_element *e = push_element(v->out);
    e->key = surface;
    e->surface = surface;
    e->texture = texture;
    e->src = src;
    e->transform = transform;
    e->box = box;
    e->opacity = v->opacity;
    e->linear = !is_one_to_one(&e->src, &box, e->transform);
    e->origin_x = (lx - p->origin_x) * p->scale;
    e->origin_y = (ly - p->origin_y) * p->scale;
    e->scale_x = width > 0 ? box.width / width : p->scale;
    e->scale_y = height > 0 ? box.height / height : p->scale;
    apply_clip(e, &v->clip);
    const pixman_region32_t *blur = vela_blur_region(surface);
    if (blur) {
        pixman_region32_t pixels;
        pixman_region32_init(&pixels);
        surface_region_to_pixels(e, blur, true, &pixels);
        if (pixman_region32_not_empty(&pixels)) {
            const pixman_box32_t *ext = pixman_region32_extents(&pixels);
            e->blur_box = (struct wlr_box) { ext->x1, ext->y1, ext->x2 - ext->x1, ext->y2 - ext->y1 };
        }
        pixman_region32_fini(&pixels);
    }
}

static void visit(struct vela_scene *scene, struct vela_node *node, double lx, double ly, float opacity,
    const struct vela_build_params *p, struct vela_elements *out, bool is_root, struct clip clip)
{
    bool captured = is_root && p->capture_root;
    if (!node->enabled && !captured) {
        return;
    }
    lx += node->x;
    ly += node->y;
    if (!captured) {
        opacity *= node->opacity;
    }
    if (opacity <= 0.0f) {
        return;
    }

    switch (node->type) {
    case VELA_NODE_TREE: {
        struct vela_tree *tree = (struct vela_tree *)node;
        if (node->unclipped) {
            clip = (struct clip) { 0 };
        }
        const struct vela_shape *shape = &tree->shape;
        if (shape->enabled && shape->width > 0.0 && shape->height > 0.0) {
            struct wlr_box rect = to_pixels(lx + shape->x, ly + shape->y, shape->width, shape->height, p);
            float radius = (float)(shape->radius * p->scale);
            // The shadow has no place in captures of a window.
            if (shape->shadow && !captured) {
                add_shadows(tree, &rect, radius, opacity, shape->active, p, out);
            }
            clip = (struct clip) { rect, radius };
        }
        struct vela_node *child;
        wl_list_for_each (child, &tree->children, link) {
            visit(scene, child, lx, ly, opacity, p, out, false, clip);
        }
        break;
    }
    case VELA_NODE_SURFACE: {
        struct wlr_surface *root = ((struct vela_surface_node *)node)->surface;
        if (!root->mapped) {
            break;
        }
        struct visit context = { scene, p, out, lx, ly, opacity, clip };
        vela_for_each_surface(root, add_surface, &context);
        break;
    }
    case VELA_NODE_RECT: {
        struct vela_rect_node *rect = (struct vela_rect_node *)node;
        struct wlr_box box = to_pixels(lx, ly, rect->width, rect->height, p);
        if (!on_screen(&box, p) || rect->color.a <= 0.0f) {
            break;
        }
        struct vela_element *e = push_element(out);
        e->key = node;
        e->color = rect->color;
        e->color.r *= opacity;
        e->color.g *= opacity;
        e->color.b *= opacity;
        e->color.a *= opacity;
        e->box = box;
        e->opacity = 1.0f;
        apply_clip(e, &clip);
        break;
    }
    case VELA_NODE_BUFFER: {
        struct vela_buffer_node *buffer = (struct vela_buffer_node *)node;
        struct wlr_box box = to_pixels(lx, ly, buffer->width, buffer->height, p);
        snap_to_buffer(&box, &buffer->src, buffer->transform, buffer->width, buffer->height, p->scale);
        if (!on_screen(&box, p) || !buffer->texture) {
            break;
        }
        struct vela_element *e = push_element(out);
        e->key = node;
        e->texture = buffer->texture;
        e->src = buffer->src;
        e->transform = buffer->transform;
        e->box = box;
        e->opacity = opacity;
        e->linear = !is_one_to_one(&e->src, &box, e->transform);
        apply_clip(e, &clip);
        break;
    }
    }
}

void vela_build_elements(struct vela_scene *scene, struct vela_node *root, const struct vela_build_params *params,
    struct vela_elements *out)
{
    out->count = 0;
    double lx = 0.0;
    double ly = 0.0;
    if (root->parent) {
        vela_node_coords(&root->parent->node, &lx, &ly);
    }
    visit(scene, root, lx, ly, 1.0f, params, out, true, (struct clip) { 0 });
}

// ------------------------------------------------------------- occlusion --

// Rounded corners aren't opaque: four squares are removed.
static void subtract_corners(const struct vela_element *e, pixman_region32_t *region)
{
    if (e->shape_radius <= 0.0f) {
        return;
    }
    const struct wlr_box *r = &e->shape_rect;
    pixman_region32_intersect_rect(region, region, r->x, r->y, (unsigned)r->width, (unsigned)r->height);
    int c = (int)ceilf(e->shape_radius);
    pixman_region32_t corners;
    pixman_region32_init(&corners);
    pixman_region32_union_rect(&corners, &corners, r->x, r->y, (unsigned)c, (unsigned)c);
    pixman_region32_union_rect(&corners, &corners, r->x + r->width - c, r->y, (unsigned)c, (unsigned)c);
    pixman_region32_union_rect(&corners, &corners, r->x, r->y + r->height - c, (unsigned)c, (unsigned)c);
    pixman_region32_union_rect(&corners, &corners, r->x + r->width - c, r->y + r->height - c, (unsigned)c,
        (unsigned)c);
    pixman_region32_subtract(region, region, &corners);
    pixman_region32_fini(&corners);
}

// The surely opaque part of an element, in pixels.
static void opaque_region(const struct vela_element *e, pixman_region32_t *out)
{
    pixman_region32_clear(out);
    if (e->opacity < 1.0f || e->shadow_sigma > 0.0f) {
        return;
    }
    if (!e->texture) {
        if (e->color.a >= 1.0f) {
            union_box(out, &e->box);
        }
        subtract_corners(e, out);
        return;
    }
    if (!e->surface) {
        if (vela_texture_is_opaque(e->texture)) {
            union_box(out, &e->box);
        }
        subtract_corners(e, out);
        return;
    }
    surface_region_to_pixels(e, &e->surface->opaque_region, false, out); // what the app declares opaque
    subtract_corners(e, out);
}

// Drops what is covered by opaque surfaces (top to bottom).
static void cull_occluded(struct vela_elements *elements)
{
    pixman_region32_t covered, visible, opaque;
    pixman_region32_init(&covered);
    pixman_region32_init(&opaque);
    for (int i = elements->count - 1; i >= 0; --i) {
        struct vela_element *e = &elements->items[i];
        pixman_region32_init_rect(&visible, e->box.x, e->box.y, (unsigned)e->box.width, (unsigned)e->box.height);
        pixman_region32_subtract(&visible, &visible, &covered);
        e->visible = pixman_region32_not_empty(&visible);
        e->visible_area = e->visible ? region_area(&visible) : 0;
        if (e->visible) {
            opaque_region(e, &opaque);
            pixman_region32_union(&covered, &covered, &opaque);
        }
        pixman_region32_fini(&visible);
    }
    pixman_region32_fini(&covered);
    pixman_region32_fini(&opaque);
}

// Blurred zones read what lies around them: if the damage touches one (within
// the blur radius) it's redrawn whole, radius included.
static void expand_damage_for_blur(const struct vela_elements *elements, pixman_region32_t *damage)
{
    // Several rounds: an enlarged zone can touch another (Start menu over the
    // taskbar).
    for (int round = 0; round < 3; ++round) {
        bool grown = false;
        for (int i = 0; i < elements->count; ++i) {
            const struct vela_element *e = &elements->items[i];
            if (!e->visible || wlr_box_empty(&e->blur_box)) {
                continue;
            }
            int reach = vela_blur_reach(blur_strength(e->scale_x));
            const pixman_box32_t zone = { e->blur_box.x - reach, e->blur_box.y - reach,
                e->blur_box.x + e->blur_box.width + reach, e->blur_box.y + e->blur_box.height + reach };
            pixman_region32_t inside;
            pixman_region32_init_rect(&inside, zone.x1, zone.y1, (unsigned)(zone.x2 - zone.x1),
                (unsigned)(zone.y2 - zone.y1));
            pixman_region32_intersect(&inside, &inside, damage);
            bool touched = pixman_region32_not_empty(&inside);
            pixman_region32_fini(&inside);
            if (touched && pixman_region32_contains_rectangle(damage, &zone) != PIXMAN_REGION_IN) {
                pixman_region32_union_rect(damage, damage, zone.x1, zone.y1, (unsigned)(zone.x2 - zone.x1),
                    (unsigned)(zone.y2 - zone.y1));
                grown = true;
            }
        }
        if (!grown) {
            return;
        }
    }
}

// Surface content changes with commits, which bring their exact damage: here
// only where and how they're drawn matters. For our own nodes the texture too.
static bool same_look(const struct vela_element *a, const struct vela_element *b)
{
    bool same_texture = a->surface ? true : a->texture == b->texture;
    return a->surface == b->surface && same_texture && same_box(&a->box, &b->box) && same_fbox(&a->src, &b->src)
        && a->transform == b->transform && a->opacity == b->opacity && a->linear == b->linear
        && same_color(&a->color, &b->color) && same_box(&a->shape_rect, &b->shape_rect)
        && a->shape_radius == b->shape_radius && same_box(&a->shadow_window, &b->shadow_window)
        && a->shadow_sigma == b->shadow_sigma && same_box(&a->blur_box, &b->blur_box);
}

// ---------------------------------------------------------------- drawing --

void vela_draw_elements(struct vela_scene *scene, struct vela_pass *pass, const struct vela_elements *elements,
    const pixman_region32_t *clip, enum wl_output_transform output_transform, int width, int height)
{
    enum wl_output_transform to_buffer = wlr_output_transform_invert(output_transform);
    for (int i = 0; i < elements->count; ++i) {
        const struct vela_element *e = &elements->items[i];
        if (!e->visible) {
            continue;
        }
        struct wlr_box dst;
        wlr_box_transform(&dst, &e->box, to_buffer, width, height);
        struct wlr_box shape = { 0 };
        if (e->shape_radius > 0.0f) {
            wlr_box_transform(&shape, &e->shape_rect, to_buffer, width, height);
        }
        if (e->shadow_sigma > 0.0f) {
            struct wlr_box window;
            wlr_box_transform(&window, &e->shadow_window, to_buffer, width, height);
            // Under the window the shadow isn't visible: the shader works only
            // on the frame around it (the corners stay, they're rounded).
            pixman_region32_t ring;
            pixman_region32_init_rect(&ring, dst.x, dst.y, (unsigned)dst.width, (unsigned)dst.height);
            if (clip) {
                pixman_region32_intersect(&ring, &ring, clip);
            }
            int inset = (int)ceilf(e->shape_radius);
            if (window.width > 2 * inset && window.height > 2 * inset) {
                pixman_region32_t inner;
                pixman_region32_init_rect(&inner, window.x + inset, window.y, (unsigned)(window.width - 2 * inset),
                    (unsigned)window.height);
                pixman_region32_union_rect(&inner, &inner, window.x, window.y + inset, (unsigned)window.width,
                    (unsigned)(window.height - 2 * inset));
                pixman_region32_subtract(&ring, &ring, &inner);
                pixman_region32_fini(&inner);
            }
            vela_pass_add_shadow(pass, &dst, &shape, &window, e->shape_radius, e->shadow_sigma, &e->color, &ring);
            pixman_region32_fini(&ring);
            continue;
        }
        if (!e->texture) {
            vela_pass_add_rect(pass, &dst, &e->color, clip, true, &shape, e->shape_radius);
            continue;
        }
        // wlroots' texture transform and box transform use inverse mappings:
        // match its software/hardware cursor rendering, which transforms the
        // box by to_buffer and passes output_transform for the texture.
        struct vela_texture_draw draw = {
            .texture = e->texture,
            .src = e->src,
            .dst = dst,
            .transform = wlr_output_transform_compose(e->transform, output_transform),
            .alpha = e->opacity,
            .linear = e->linear,
            .blend = true,
            .clip = clip,
            .shape_rect = shape,
            .shape_radius = e->shape_radius,
        };
        // Explicit sync: the buffer is ready when the app's acquire point
        // signals.
        if (e->surface) {
            struct wlr_linux_drm_syncobj_surface_v1_state *sync
                = wlr_linux_drm_syncobj_v1_get_surface_state(e->surface);
            if (sync && sync->acquire_timeline) {
                draw.wait_timeline = sync->acquire_timeline;
                draw.wait_point = sync->acquire_point;
            }
        }
        // Under the surface, the blurred background it asked for (§8.3).
        const pixman_region32_t *blur = e->surface && !wlr_box_empty(&e->blur_box) ? vela_blur_region(e->surface)
                                                                                   : NULL;
        if (blur) {
            pixman_region32_t region;
            pixman_region32_init(&region);
            surface_region_to_pixels(e, blur, true, &region);
            wlr_region_transform(&region, &region, to_buffer, width, height);
            if (clip) {
                pixman_region32_intersect(&region, &region, clip);
            }
            if (pixman_region32_not_empty(&region)) {
                struct vela_texture_draw panel = draw;
                panel.clip = NULL;
                vela_pass_add_blur(pass, &panel, &region, blur_strength(e->scale_x), &scene->acrylic_tint);
            }
            pixman_region32_fini(&region);
        }
        vela_pass_add_texture(pass, &draw);
    }
}

void vela_add_release_points(struct vela_scene *scene, const struct vela_elements *elements,
    struct vela_renderer *renderer, uint64_t sync_point)
{
    struct wlr_drm_syncobj_timeline *timeline = vela_renderer_sync_timeline(renderer);
    if (!timeline || sync_point == 0 || !scene->event_loop) {
        return;
    }
    for (int i = 0; i < elements->count; ++i) {
        const struct vela_element *e = &elements->items[i];
        if (!e->visible || !e->surface) {
            continue;
        }
        struct wlr_linux_drm_syncobj_surface_v1_state *sync = wlr_linux_drm_syncobj_v1_get_surface_state(e->surface);
        if (sync) {
            wlr_linux_drm_syncobj_v1_state_add_release_point(sync, timeline, sync_point, scene->event_loop);
        }
    }
}

// ----------------------------------------------------------------- output --

static void handle_damage(struct wl_listener *listener, void *data)
{
    struct vela_output_frame *frame = wl_container_of(listener, frame, damage);
    const struct wlr_output_event_damage *event = data;
    // Upright output coordinates, already like scene_damage: wlroots' header
    // calls them buffer-local, but its cursors emit their box as is, and its
    // own scene converts that damage to the buffer as we do in render(), once.
    pixman_region32_union(&frame->scene_damage, &frame->scene_damage, event->damage);
    vela_output_schedule_frame(frame->owner);
}

static void handle_needs_frame(struct wl_listener *listener, void *data)
{
    struct vela_output_frame *frame = wl_container_of(listener, frame, needs_frame);
    vela_output_schedule_frame(frame->owner);
}

struct vela_output_frame *vela_output_frame_create(struct vela_scene *scene, struct vela_renderer *renderer,
    struct wlr_output *output, struct vela_output *owner)
{
    struct vela_output_frame *frame = calloc(1, sizeof(*frame));
    frame->scene = scene;
    frame->renderer = renderer;
    frame->output = output;
    frame->owner = owner;
    frame->zoom = 1.0;
    wlr_damage_ring_init(&frame->ring);
    pixman_region32_init(&frame->scene_damage);
    wl_list_insert(scene->frames.prev, &frame->link);

    // The cursor we draw (when there is no hardware one) and whoever asks for
    // a frame (hardware cursor, captures) come through here.
    vela_listen(&output->events.damage, &frame->damage, handle_damage);
    vela_listen(&output->events.needs_frame, &frame->needs_frame, handle_needs_frame);

    // Scanout of buffers with explicit sync needs a backend that can wait for
    // and signal timelines (DRM, nested in a host with linux-drm-syncobj). The
    // headless one claims it can, but then refuses commits with timelines.
    if (output->backend->features.timeline && vela_renderer_sync_timeline(renderer)
        && !wlr_output_is_headless(output)) {
        frame->scanout_timeline = wlr_drm_syncobj_timeline_create(vela_renderer_render_fd(renderer));
    }
    return frame;
}

static void send_feedback(struct vela_output_frame *frame, struct wlr_surface *surface, bool scanout);

void vela_output_frame_destroy(struct vela_output_frame *frame)
{
    wl_list_remove(&frame->damage.link);
    wl_list_remove(&frame->needs_frame.link);
    wl_list_remove(&frame->link);
    for (int i = 0; i < frame->last.count; ++i) {
        if (frame->last.items[i].surface) {
            vela_surface_forget(frame->last.items[i].surface, frame->output);
        }
    }
    if (frame->feedback_surface) {
        send_feedback(frame, frame->feedback_surface, false);
    }
    if (frame->night_transform) {
        wlr_color_transform_unref(frame->night_transform);
    }
    if (frame->scanout_timeline) {
        wlr_drm_syncobj_timeline_unref(frame->scanout_timeline);
    }
    wlr_damage_ring_finish(&frame->ring);
    pixman_region32_fini(&frame->scene_damage);
    free(frame->last.items);
    free(frame->current.items);
    free(frame->found);
    free(frame->visible_surfaces);
    free(frame);
}

/**
 * Select the DRM gamma LUT or Vulkan shader for the next output commit.
 * If the shader path is forced, first remove an installed LUT to prevent
 * applying the same warm tint twice.
 */
static void prepare_night_light(struct vela_output_frame *frame)
{
    struct vela_scene *scene = frame->scene;
    if (scene->night_version == frame->night_version && !frame->gamma_retry_pending) {
        return;
    }
    bool was_shader = scene->night_active && !frame->night_in_gamma;
    frame->night_version = scene->night_version;
    // Only real DRM outputs have a gamma LUT. An earlier failure suppresses
    // repeated tests during the animation, except on an explicit retry.
    if (!wlr_output_is_drm(frame->output)) {
        frame->night_in_gamma = false;
        return;
    }
    if (scene->night_force_shader) {
        // Never submit a new gamma LUT in this mode. If the hardware still
        // holds an old one, remove it atomically with the shader-tinted frame.
        // A failed NULL commit must never let a shader-tinted buffer through
        // the still-active LUT (double tint).
        if (frame->night_transform) {
            wlr_color_transform_unref(frame->night_transform);
            frame->night_transform = NULL;
        }
        bool keep_old_lut = vela_night_light_defer_clear(frame->gamma_refused, frame->night_clear_needed,
            frame->gamma_retry_pending, frame->night_gamma_committed);
        frame->gamma_retry_pending = false;
        frame->night_clear_needed = frame->night_gamma_committed;
        frame->night_commit_pending = frame->night_clear_needed && !keep_old_lut;
        frame->night_in_gamma = keep_old_lut;
        if (scene->night_active || frame->night_clear_needed) {
            vela_output_frame_damage_whole(frame);
        }
        return;
    }
    if (frame->gamma_refused && !frame->gamma_retry_pending) {
        // If clearing the old LUT failed, keep drawing without a shader:
        // otherwise its still-committed LUT would apply a second tint.
        frame->night_in_gamma = frame->night_clear_needed && frame->night_gamma_committed
            && !frame->night_commit_pending;
        return;
    }
    struct wlr_color_transform *transform = scene->night_active ? night_lut(scene->night_gains) : NULL;
    struct wlr_output_state test;
    wlr_output_state_init(&test);
    wlr_output_state_set_color_transform(&test, transform);
    bool ok = wlr_output_test_state(frame->output, &test);
    wlr_output_state_finish(&test);
    if (!ok) {
        wlr_log(WLR_INFO, "%s: the monitor doesn't accept night light in the gamma LUT, rendering applies it",
            frame->output->name);
        if (transform) {
            wlr_color_transform_unref(transform);
        }
        bool was_refused = frame->gamma_refused;
        frame->gamma_refused = true;
        frame->gamma_retry_pending = false;
        frame->night_in_gamma = false;
        if (frame->night_transform) {
            wlr_color_transform_unref(frame->night_transform);
            frame->night_transform = NULL;
        }
        // Removing the old LUT is an explicit color commit. Omitting the
        // transform from the state leaves the hardware LUT installed.
        frame->night_clear_needed = frame->night_gamma_committed;
        frame->night_commit_pending = frame->night_clear_needed;
        if (scene->night_active && (!was_refused || frame->night_clear_needed)) {
            // The shader path changes all pixels. A previously failed clear
            // also needs a fresh repaint when it is attempted again.
            vela_output_frame_damage_whole(frame);
        }
        return;
    }
    bool recovering = frame->gamma_refused;
    frame->gamma_refused = false;
    frame->gamma_retry_pending = false;
    if (frame->night_transform) {
        wlr_color_transform_unref(frame->night_transform);
    }
    frame->night_transform = transform;
    frame->night_commit_pending = true;
    frame->night_in_gamma = scene->night_active;
    if ((recovering || was_shader) && scene->night_active) {
        // Previously rendered buffers may contain the shader's warm tint.
        // Redraw them once without the shader when the LUT comes back.
        vela_output_frame_damage_whole(frame);
    }
}

// Only a rejected color commit enters this path; ordinary buffer/render
// failures must not poison gamma support. A rejected new LUT falls back to
// Vulkan on the next frame. If a LUT is already installed, first attempt to
// clear it atomically with the Vulkan frame. A rejected NULL commit is
// deferred until the next explicit retry instead of spinning every frame.
static void night_color_commit_failed(struct vela_output_frame *frame)
{
    bool tried_lut = frame->night_transform != NULL;
    if (frame->night_transform) {
        wlr_color_transform_unref(frame->night_transform);
        frame->night_transform = NULL;
    }
    frame->gamma_refused = true;
    frame->gamma_retry_pending = false;
    frame->night_clear_needed = frame->night_gamma_committed;
    frame->night_commit_pending = tried_lut && frame->night_clear_needed;
    frame->night_in_gamma = frame->night_clear_needed && !frame->night_commit_pending;
    wlr_log(WLR_INFO, "%s: gamma color commit rejected, %s", frame->output->name,
        frame->night_commit_pending ? "clearing previous LUT before Vulkan fallback"
                                    : frame->night_clear_needed ? "deferring LUT clear until retry"
                                                                : "falling back to Vulkan");
    vela_output_frame_damage_whole(frame);
}

void vela_output_frame_set_magnifier(struct vela_output_frame *frame, double zoom, double x, double y)
{
    zoom = zoom > 1.0 ? zoom : 1.0;
    if (zoom == frame->zoom && (zoom == 1.0 || (x == frame->zoom_x && y == frame->zoom_y))) {
        return;
    }
    frame->zoom = zoom;
    frame->zoom_x = x;
    frame->zoom_y = y;
    // Everything moves: comparing with the previous frame finds the damage.
    vela_output_schedule_frame(frame->owner);
}

void vela_output_frame_damage_whole(struct vela_output_frame *frame)
{
    int width = 0;
    int height = 0;
    wlr_output_transformed_resolution(frame->output, &width, &height);
    const struct wlr_box whole = { 0, 0, width, height };
    union_box(&frame->scene_damage, &whole);
    vela_output_schedule_frame(frame->owner);
}

/** Request fresh pixels for shader color changes or a gamma-only commit. */
void vela_output_frame_color_changed(struct vela_output_frame *frame, bool filter_changed, bool night_changed)
{
    if (vela_night_light_repaint_required(filter_changed, night_changed, wlr_output_is_drm(frame->output),
            frame->scene->night_force_shader, frame->gamma_refused, frame->night_in_gamma)) {
        vela_output_frame_damage_whole(frame);
    } else if (night_changed) {
        // A DRM gamma transform can be committed with unchanged pixels.
        vela_output_schedule_frame(frame->owner);
    }
}

/** Permit one more hardware color commit after an earlier gamma LUT rejection. */
void vela_output_frame_retry_night_light(struct vela_output_frame *frame)
{
    if ((!frame->gamma_refused && !frame->night_clear_needed) || frame->gamma_retry_pending
        || !wlr_output_is_drm(frame->output) || !frame->output->enabled) {
        return;
    }
    // The retry flag bypasses the version guard without inventing a version.
    frame->gamma_retry_pending = true;
    vela_output_schedule_frame(frame->owner);
}

static struct vela_element *last_element(struct vela_output_frame *frame, const void *key)
{
    for (int i = 0; i < frame->last.count; ++i) {
        if (frame->last.items[i].key == key) {
            return &frame->last.items[i];
        }
    }
    return NULL;
}

bool vela_output_frame_shows(struct vela_output_frame *frame, struct wlr_surface *surface)
{
    return last_element(frame, surface) != NULL;
}

void vela_output_frame_surface_committed(struct vela_output_frame *frame, struct wlr_surface *surface)
{
    const struct vela_element *e = last_element(frame, surface);
    if (!e) {
        return;
    }
    pixman_region32_t region;
    pixman_region32_init(&region);
    wlr_surface_get_effective_damage(surface, &region);
    if (pixman_region32_not_empty(&region)) {
        bool exact = e->scale_x == floor(e->scale_x) && e->scale_y == floor(e->scale_y)
            && e->origin_x == floor(e->origin_x) && e->origin_y == floor(e->origin_y);
        wlr_region_scale_xy(&region, &region, (float)e->scale_x, (float)e->scale_y);
        pixman_region32_translate(&region, (int)floor(e->origin_x), (int)floor(e->origin_y));
        if (!exact) {
            wlr_region_expand(&region, &region, 1); // the filter touches the neighbors too
        }
        pixman_region32_intersect_rect(&region, &region, e->box.x, e->box.y, (unsigned)e->box.width,
            (unsigned)e->box.height);
        pixman_region32_union(&frame->scene_damage, &frame->scene_damage, &region);
    }
    pixman_region32_fini(&region);
    // Even without damage: the app waits for its frame callback.
    vela_output_schedule_frame(frame->owner);
}

void vela_output_frame_surface_destroyed(struct vela_output_frame *frame, struct wlr_surface *surface)
{
    if (surface == frame->feedback_surface) {
        frame->feedback_surface = NULL;
        frame->feedback_debounce = 0;
    }
    struct vela_element *e = last_element(frame, surface);
    if (!e) {
        return;
    }
    union_box(&frame->scene_damage, &e->box);
    // Out of the previous frame (duplicates included).
    int kept = 0;
    for (int i = 0; i < frame->last.count; ++i) {
        if (frame->last.items[i].key != surface) {
            frame->last.items[kept++] = frame->last.items[i];
        }
    }
    frame->last.count = kept;
    kept = 0;
    for (int i = 0; i < frame->visible_count; ++i) {
        if (frame->visible_surfaces[i] != surface) {
            frame->visible_surfaces[kept++] = frame->visible_surfaces[i];
        }
    }
    frame->visible_count = kept;
    vela_output_schedule_frame(frame->owner);
}

static void update_surfaces(struct vela_output_frame *frame, const struct vela_elements *elements)
{
    frame->visible_count = 0;
    const struct wlr_box bounds = { 0, 0, frame->width, frame->height };
    for (int i = 0; i < elements->count; ++i) {
        const struct vela_element *e = &elements->items[i];
        if (!e->surface) {
            continue;
        }
        struct wlr_box on_output = { 0 };
        wlr_box_intersection(&on_output, &e->box, &bounds);
        vela_surface_report(e->surface, frame->output, (int64_t)on_output.width * on_output.height, e->visible_area);
        if (e->visible) {
            frame->visible_surfaces = vela_grow(frame->visible_surfaces, &frame->visible_capacity,
                frame->visible_count + 1, sizeof(*frame->visible_surfaces));
            frame->visible_surfaces[frame->visible_count++] = e->surface;
        }
    }
}

void vela_output_frame_reset_damage(struct vela_output_frame *frame)
{
    // All buffers start over: the next frame redraws them whole.
    wlr_damage_ring_finish(&frame->ring);
    wlr_damage_ring_init(&frame->ring);
    frame->width = 0;
    frame->height = 0;
    pixman_region32_clear(&frame->scene_damage);
    vela_output_schedule_frame(frame->owner);
}

// The damage: what appeared, disappeared, changed or moved compared with the
// previous frame (surface content comes from commits). Then `current` becomes
// `last`.
static void diff_with_last(struct vela_output_frame *frame)
{
    struct vela_elements *current = &frame->current;
    struct vela_elements *last = &frame->last;
    pixman_region32_t changed;
    pixman_region32_init(&changed);
    // Which earlier elements are found now; the search starts where they're
    // expected (usually the order doesn't change).
    frame->found = vela_grow(frame->found, &frame->found_capacity, last->count, sizeof(*frame->found));
    bool *found = frame->found;
    if (last->count > 0) {
        memset(found, 0, (size_t)last->count * sizeof(*found));
    }
    int highest_old_order = 0;
    for (int i = 0; i < current->count; ++i) {
        struct vela_element *e = &current->items[i];
        e->order = i;
        const struct vela_element *old = NULL;
        for (int k = 0; k < last->count; ++k) {
            int j = (i + k) % last->count;
            if (last->items[j].key == e->key) {
                old = &last->items[j];
                found[j] = true;
                break;
            }
        }
        if (!old) {
            union_box(&changed, &e->box);
            continue;
        }
        if (!same_look(e, old)) {
            union_box(&changed, &old->box);
            union_box(&changed, &e->box);
        } else if (old->order < highest_old_order) {
            union_box(&changed, &e->box); // it rose above other elements
        }
        if (old->order > highest_old_order) {
            highest_old_order = old->order;
        }
    }
    for (int j = 0; j < last->count; ++j) {
        if (found[j]) {
            continue;
        }
        union_box(&changed, &last->items[j].box);
        if (last->items[j].surface) {
            vela_surface_forget(last->items[j].surface, frame->output);
        }
    }
    pixman_region32_union(&frame->scene_damage, &frame->scene_damage, &changed);
    pixman_region32_fini(&changed);

    struct vela_elements swap = *last;
    *last = *current;
    *current = swap;
}

static const struct vela_element *scanout_candidate(struct vela_output_frame *frame,
    const struct vela_elements *elements, enum wl_output_transform transform);
static bool try_scanout(struct vela_output_frame *frame, const struct vela_element *e, struct wlr_output_state *state);
static void update_feedback(struct vela_output_frame *frame, const struct vela_element *candidate);

bool vela_output_frame_render(struct vela_output_frame *frame, double lx, double ly, struct wlr_output_state *pending)
{
    struct wlr_output *output = frame->output;
    struct vela_scene *scene = frame->scene;
    if (!output->enabled) {
        return false;
    }
    // The frame's size and scale: the new ones, if the commit changes mode.
    int width = output->width;
    int height = output->height;
    float scale = output->scale;
    enum wl_output_transform transform = output->transform;
    if (pending && (pending->committed & WLR_OUTPUT_STATE_MODE)) {
        if (pending->mode_type == WLR_OUTPUT_STATE_MODE_FIXED && pending->mode) {
            width = pending->mode->width;
            height = pending->mode->height;
        } else {
            width = pending->custom_mode.width;
            height = pending->custom_mode.height;
        }
    }
    if (pending && (pending->committed & WLR_OUTPUT_STATE_SCALE)) {
        scale = pending->scale;
    }
    if (pending && (pending->committed & WLR_OUTPUT_STATE_TRANSFORM)) {
        transform = pending->transform;
    }
    if (transform & WL_OUTPUT_TRANSFORM_90) {
        int swap = width;
        width = height;
        height = swap;
    }
    const struct wlr_box whole = { 0, 0, width, height };
    // Even a 90 -> 270 change with identical dimensions invalidates buffer-age
    // history: the old buffers contain pixels in a different orientation.
    if (width != frame->width || height != frame->height || scale != frame->scale
        || transform != frame->transform) {
        wlr_damage_ring_finish(&frame->ring);
        wlr_damage_ring_init(&frame->ring);
        frame->width = width;
        frame->height = height;
        frame->scale = scale;
        frame->transform = transform;
        pixman_region32_clear(&frame->scene_damage);
        union_box(&frame->scene_damage, &whole);
    }

    // 1. The flattened scene, in this output's pixels (with the magnifier: the
    //    zoomed area), without what is covered.
    bool magnified = frame->zoom > 1.0;
    const struct vela_build_params params = {
        .origin_x = magnified ? frame->zoom_x : lx,
        .origin_y = magnified ? frame->zoom_y : ly,
        .scale = scale * (magnified ? frame->zoom : 1.0),
        .bounds = whole,
    };
    vela_build_elements(scene, &scene->root->node, &params, &frame->current);
    cull_occluded(&frame->current);

    // 2. The damage compared with the previous frame; from here on this
    //    frame's elements are in frame->last.
    diff_with_last(frame);
    const struct vela_elements *elements = &frame->last;
    expand_damage_for_blur(elements, &frame->scene_damage);
    // Diagnostics: VELA_DEBUG_DAMAGE=1 redraws everything on every frame.
    if (vela_env_one("VELA_DEBUG_DAMAGE") && (pixman_region32_not_empty(&frame->scene_damage)
            || pixman_region32_not_empty(&frame->ring.current))) {
        wlr_damage_ring_finish(&frame->ring);
        wlr_damage_ring_init(&frame->ring);
        union_box(&frame->scene_damage, &whole);
    }
    update_surfaces(frame, elements);

    // Color: filters, and night light when the monitor's gamma won't take it,
    // are applied while drawing (cursor included).
    prepare_night_light(frame);
    bool night_in_drawing = scene->night_active && !frame->night_in_gamma;
    bool filtered = scene->color_filtered || night_in_drawing;
    float filter[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    if (filtered) {
        const float *f = scene->color_filtered ? scene->color_filter : filter;
        float base[9];
        memcpy(base, f, sizeof(base));
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                filter[row * 3 + column] = (night_in_drawing ? scene->night_gains[row] : 1.0f) * base[row * 3 + column];
            }
        }
    }
    if (filtered != frame->cursor_locked) {
        wlr_output_lock_software_cursors(output, filtered);
        frame->cursor_locked = filtered;
    }

    // A fullscreen app that can be shown as it is (not with the filter in
    // drawing).
    const struct vela_element *candidate = pending || filtered ? NULL : scanout_candidate(frame, elements, transform);
    update_feedback(frame, candidate);

    bool damaged = pixman_region32_not_empty(&frame->scene_damage)
        || pixman_region32_not_empty(&frame->ring.current) || frame->night_commit_pending;
    if (!damaged && !output->needs_frame && !pending) {
        return false;
    }
    frame->delivered = (struct vela_frame_delivered) { 0, -1, false, false };

    // The commit's state: the caller's (mode change, drawn at once at the new
    // size) or one of ours.
    struct wlr_output_state own;
    wlr_output_state_init(&own);
    struct wlr_output_state *state = pending ? pending : &own;
    if (frame->night_commit_pending) {
        wlr_output_state_set_color_transform(state, frame->night_transform);
    }
    // A buffer is delivered even when only the hardware cursor moves (with
    // empty damage drawing costs almost nothing). With DRM a commit without a
    // buffer blocks: it would stop the compositor until this output's vblank,
    // and meanwhile the other outputs would miss theirs (with a 75 Hz next to
    // it, the 180 Hz dropped below 60 fps).

    // Direct scanout: no drawing, the app's buffer goes on screen.
    if (candidate && try_scanout(frame, candidate, state)) {
        wlr_output_state_finish(&own);
        if (frame->night_commit_pending) {
            frame->night_gamma_committed = frame->night_transform != NULL;
            frame->night_clear_needed = false;
        }
        frame->night_commit_pending = false;
        // The output is up to date: the accumulated damage is no longer needed
        // (our buffers, instead, fell behind; see below).
        pixman_region32_clear(&frame->ring.current);
        pixman_region32_clear(&frame->scene_damage);
        if (!frame->scanout) {
            wlr_log(WLR_INFO, "%s: direct scanout on", output->name);
        }
        frame->scanout = true;
        frame->delivered.scanout = true;
        return true;
    }
    if (frame->scanout) {
        // Back to compositing: our buffers haven't seen the scanout frames,
        // they're redrawn whole.
        wlr_log(WLR_INFO, "%s: direct scanout off", output->name);
        frame->scanout = false;
        if (frame->tearing) {
            wlr_log(WLR_INFO, "%s: tearing off", output->name);
            frame->tearing = false;
        }
        union_box(&frame->scene_damage, &whole);
    }

    if (!wlr_output_configure_primary_swapchain(output, state, &output->swapchain)) {
        wlr_output_state_finish(&own);
        return false;
    }
    struct wlr_buffer *buffer = wlr_swapchain_acquire(output->swapchain);
    if (!buffer) {
        wlr_output_state_finish(&own);
        return false;
    }

    // This frame's damage (for the backend) and that of the buffer we're about
    // to reuse (for the buffer age: what it lacks compared with now).
    pixman_region32_t frame_damage, buffer_damage;
    pixman_region32_init(&frame_damage);
    pixman_region32_init(&buffer_damage);
    enum wl_output_transform to_buffer = wlr_output_transform_invert(transform);
    // The ring requires buffer coordinates, including its full damage for a
    // newly acquired buffer. Never rotate that implicit full damage as though
    // it were upright: a rectangular portrait output would lose a strip.
    wlr_region_transform(&frame_damage, &frame->scene_damage, to_buffer, width, height);
    wlr_damage_ring_add(&frame->ring, &frame_damage);
    pixman_region32_clear(&frame->scene_damage);
    pixman_region32_copy(&frame_damage, &frame->ring.current);
    wlr_damage_ring_rotate_buffer(&frame->ring, buffer, &buffer_damage);
    // Blur dependencies live in upright scene coordinates.
    wlr_region_transform(&buffer_damage, &buffer_damage, transform, buffer->width, buffer->height);
    expand_damage_for_blur(elements, &buffer_damage);
    wlr_region_transform(&buffer_damage, &buffer_damage, to_buffer, width, height);

    // The damage ring may contain rectangles outside the output (for example
    // when a window crosses a monitor edge). Only valid buffer coordinates
    // must reach the renderer and wlroots' screencopy path.
    pixman_region32_intersect_rect(&buffer_damage, &buffer_damage, 0, 0, buffer->width, buffer->height);
    pixman_region32_intersect_rect(&frame_damage, &frame_damage, 0, 0, buffer->width, buffer->height);

    bool ok = false;
    bool color_commit_failed = false;
    struct vela_pass *pass = vela_renderer_begin_pass(frame->renderer, buffer);
    if (pass) {
        if (filtered) {
            vela_pass_set_color_filter(pass, filter);
        }
        // Black below everything (the desktop wallpaper usually covers it).
        const struct wlr_box all = { 0, 0, buffer->width, buffer->height };
        const struct wlr_render_color black = { 0.0f, 0.0f, 0.0f, 1.0f };
        vela_pass_add_rect(pass, &all, &black, &buffer_damage, false, NULL, 0.0f);
        vela_draw_elements(scene, pass, elements, &buffer_damage, transform, width, height);
        wlr_output_add_software_cursors_to_render_pass(output, vela_pass_wlr(pass), &buffer_damage);
        vela_pass_measure(pass);
        struct vela_pass_result result;
        ok = vela_pass_submit(pass, &result);
        if (ok) {
            frame->delivered.point = result.point;
            frame->delivered.timing_slot = result.timing_slot;
            vela_add_release_points(scene, elements, frame->renderer, result.sync_point);
        }
    }

    if (ok) {
        wlr_output_state_set_buffer(state, buffer);
        wlr_output_state_set_damage(state, &frame_damage);
        for (int i = 0; i < frame->visible_count; ++i) {
            struct wlr_surface *surface = frame->visible_surfaces[i];
            const struct vela_surface_state *info = vela_surface_state_get(surface);
            if (!info || info->pacing == output) {
                wlr_presentation_surface_textured_on_output(surface, output);
            }
        }
        ok = wlr_output_commit_state(output, state);
        if (!ok && frame->night_commit_pending) {
            // A commit can fail for reasons unrelated to gamma. Try the same
            // buffer/mode without changing the color transform before deciding
            // that the LUT was rejected. Omitting the bit retains an old LUT.
            struct wlr_output_state no_color;
            wlr_output_state_init(&no_color);
            if (wlr_output_state_copy(&no_color, state)) {
                wlr_output_state_set_color_transform(&no_color, NULL);
                no_color.committed &= ~WLR_OUTPUT_STATE_COLOR_TRANSFORM;
                // A shader-tinted frame must never be delivered with an old
                // hardware LUT still installed: omitting a rejected NULL
                // clear would briefly apply both tints. In that case, test
                // the distinction without committing the unsafe buffer.
                bool would_double_tint = frame->night_gamma_committed
                    && frame->night_transform == NULL && night_in_drawing;
                if (!would_double_tint && wlr_output_commit_state(output, &no_color)) {
                    // This buffer did reach the output, but the requested LUT
                    // did not. Repaint with the shader and (if necessary)
                    // clear the previously installed LUT on the next frame.
                    ok = true;
                    color_commit_failed = true;
                    wlr_output_state_set_color_transform(state, NULL);
                    state->committed &= ~WLR_OUTPUT_STATE_COLOR_TRANSFORM;
                } else {
                    // If both commits fail, only blame gamma when testing
                    // isolates the color transform as the rejected change.
                    // A runtime failure despite valid tests is not proof.
                    color_commit_failed = !wlr_output_test_state(output, state)
                        && wlr_output_test_state(output, &no_color);
                }
            }
            wlr_output_state_finish(&no_color);
        }
    }
    if (ok && frame->night_commit_pending && !color_commit_failed) {
        frame->night_gamma_committed = frame->night_transform != NULL;
        frame->night_clear_needed = false;
        frame->night_commit_pending = false;
    }
    wlr_buffer_unlock(buffer);
    wlr_output_state_finish(&own);
    if (!ok) {
        // The frame didn't reach the output: its damage must be redrawn.
        wlr_damage_ring_add(&frame->ring, &frame_damage);
    }
    if (color_commit_failed) {
        // Also needed when the buffer-only retry succeeded: that success
        // does not mean the gamma LUT was applied.
        night_color_commit_failed(frame);
    }
    pixman_region32_fini(&frame_damage);
    pixman_region32_fini(&buffer_damage);
    return ok;
}

// ---------------------------------------------------------- direct scanout --

// VELA_DEBUG_SCANOUT=1: why a fullscreen app doesn't go into scanout.
static void note_scanout_reason(struct vela_output_frame *frame, const char *reason)
{
    if (vela_env_one("VELA_DEBUG_SCANOUT") && reason != frame->scanout_reason) {
        wlr_log(WLR_INFO, "%s: no direct scanout: %s", frame->output->name, reason ? reason : "candidate again");
    }
    frame->scanout_reason = reason;
}

// Direct scanout (§5.3): a single opaque surface covers the output, 1:1, and
// its buffer goes on the primary plane without drawing anything.
static const struct vela_element *scanout_candidate(struct vela_output_frame *frame,
    const struct vela_elements *elements, enum wl_output_transform transform)
{
    // VELA_SCANOUT=0: always composite (diagnostics, comparisons).
    if (vela_env_off("VELA_SCANOUT")) {
        return NULL;
    }
    const char *reason = NULL;
    const struct vela_element *found = NULL;
    for (int i = 0; i < elements->count; ++i) {
        if (!elements->items[i].visible) {
            continue;
        }
        if (found) {
            reason = "something else is visible (panels, notifications, our own cursor)";
            found = NULL;
            break;
        }
        found = &elements->items[i];
    }
    if (found) {
        const struct vela_element *e = found;
        const struct wlr_box whole = { 0, 0, frame->width, frame->height };
        if (!e->surface || !e->texture) {
            reason = "not an app surface";
        } else if (e->shape_radius > 0.0f) {
            reason = "has rounded corners";
        } else if (e->opacity < 1.0f) {
            reason = "is translucent";
        } else if (!same_box(&e->box, &whole)) {
            reason = "doesn't cover the output exactly";
        } else if (e->transform != transform) {
            reason = "is rotated relative to the output";
        } else if (e->linear) {
            reason = "is scaled (buffer size differs from the output)";
        } else {
            // Opaque: a format without alpha, or an opaque region declared
            // over everything.
            bool opaque = vela_texture_is_opaque(e->texture);
            if (!opaque) {
                pixman_box32_t all = { 0, 0, e->surface->current.width, e->surface->current.height };
                opaque = pixman_region32_contains_rectangle(&e->surface->opaque_region, &all) == PIXMAN_REGION_IN;
            }
            if (!opaque) {
                reason = "has alpha and doesn't declare itself opaque";
            }
        }
        if (reason) {
            found = NULL;
        }
    }
    note_scanout_reason(frame, reason);
    return found;
}

static bool try_scanout(struct vela_output_frame *frame, const struct vela_element *e, struct wlr_output_state *state)
{
    struct wlr_output *output = frame->output;
    struct vela_scene *scene = frame->scene;
    if (!wlr_output_is_direct_scanout_allowed(output)) {
        note_scanout_reason(frame, "our own cursor, or a screen capture in progress");
        return false;
    }
    struct wlr_surface *surface = e->surface;
    struct wlr_client_buffer *client = surface->buffer;
    if (!client) {
        note_scanout_reason(frame, "no buffer");
        return false;
    }
    struct wlr_buffer *buffer = &client->base;
    if (client->source && client->source->n_locks > 0) {
        buffer = client->source;
    }
    struct wlr_dmabuf_attributes dmabuf;
    if (!wlr_buffer_get_dmabuf(buffer, &dmabuf)) {
        // The output's plane reads only dmabufs.
        note_scanout_reason(frame, "the buffer isn't a dmabuf (shared memory)");
        return false;
    }
    if (e->src.x != 0.0 || e->src.y != 0.0 || e->src.width != (double)buffer->width
        || e->src.height != (double)buffer->height) {
        note_scanout_reason(frame, "the app shows only part of it"); // viewporter
        return false;
    }

    struct wlr_output_state attempt;
    wlr_output_state_init(&attempt);
    if (!wlr_output_state_copy(&attempt, state)) {
        return false;
    }
    wlr_output_state_set_buffer(&attempt, buffer);
    // Tearing: the app (a game) wants every frame on screen at once.
    bool tearing = scene->allow_tearing && scene->tearing_control
        && wlr_tearing_control_manager_v1_surface_hint_from_surface(scene->tearing_control, surface)
            == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
    attempt.tearing_page_flip = tearing;
    struct wlr_linux_drm_syncobj_surface_v1_state *sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface);
    if (sync && sync->acquire_timeline) {
        if (!frame->scanout_timeline) {
            wlr_output_state_finish(&attempt);
            note_scanout_reason(frame, "the app uses explicit sync, which this output can't handle (headless)");
            return false;
        }
        wlr_output_state_set_wait_timeline(&attempt, sync->acquire_timeline, sync->acquire_point);
        wlr_output_state_set_signal_timeline(&attempt, frame->scanout_timeline, frame->scanout_point + 1);
    }
    bool ok = wlr_output_test_state(output, &attempt);
    if (!ok && !tearing) {
        note_scanout_reason(frame, "the monitor (or driver) doesn't accept the buffer on its plane");
    }
    if (!ok && tearing) {
        // The output (or driver) doesn't allow it: with the vblank, as always.
        tearing = false;
        attempt.tearing_page_flip = false;
        ok = wlr_output_test_state(output, &attempt);
    }
    if (ok) {
        const struct vela_surface_state *info = vela_surface_state_get(surface);
        if (!info || info->pacing == output) {
            wlr_presentation_surface_scanned_out_on_output(surface, output);
        }
        ok = wlr_output_commit_state(output, &attempt);
    }
    if (ok && tearing != frame->tearing) {
        wlr_log(WLR_INFO, "%s: tearing %s", output->name, tearing ? "on (the app asks for it)" : "off");
        frame->tearing = tearing;
    }
    frame->delivered.tearing = ok && tearing;
    if (ok && sync && sync->acquire_timeline) {
        // The backend signals the point when it stops showing the buffer.
        ++frame->scanout_point;
        wlr_linux_drm_syncobj_v1_state_add_release_point(sync, frame->scanout_timeline, frame->scanout_point,
            output->event_loop);
    }
    wlr_output_state_finish(&attempt);
    return ok;
}

// dmabuf feedback: the primary plane's formats to the app that is a scanout
// candidate, after a few confirming frames; then the normal ones again. Like
// wlr_scene: before changing the recommended formats we wait for a run of
// frames, because recreating buffers is costly.
static void update_feedback(struct vela_output_frame *frame, const struct vela_element *candidate)
{
    enum { DEBOUNCE_FRAMES = 30 };
    if (!frame->scene->linux_dmabuf) {
        return;
    }
    struct wlr_surface *target = candidate ? candidate->surface : NULL;
    if (target && target == frame->feedback_surface) {
        frame->feedback_debounce = DEBOUNCE_FRAMES;
        return;
    }
    if (target) {
        if (++frame->feedback_debounce < DEBOUNCE_FRAMES) {
            return;
        }
        if (frame->feedback_surface) {
            send_feedback(frame, frame->feedback_surface, false);
        }
        send_feedback(frame, target, true);
        frame->feedback_surface = target;
        frame->feedback_debounce = DEBOUNCE_FRAMES;
        return;
    }
    if (!frame->feedback_surface) {
        frame->feedback_debounce = 0;
        return;
    }
    if (--frame->feedback_debounce > 0) {
        return;
    }
    send_feedback(frame, frame->feedback_surface, false);
    frame->feedback_surface = NULL;
    frame->feedback_debounce = 0;
}

static void send_feedback(struct vela_output_frame *frame, struct wlr_surface *surface, bool scanout)
{
    if (!scanout) {
        wlr_linux_dmabuf_v1_set_surface_feedback(frame->scene->linux_dmabuf, surface, NULL);
        wlr_log(WLR_DEBUG, "%s: default dmabuf feedback to a surface", frame->output->name);
        return;
    }
    const struct wlr_linux_dmabuf_feedback_v1_init_options options = {
        .main_renderer = vela_renderer_wlr(frame->renderer),
        .scanout_primary_output = frame->output,
    };
    struct wlr_linux_dmabuf_feedback_v1 feedback = { 0 };
    if (!wlr_linux_dmabuf_feedback_v1_init_with_options(&feedback, &options)) {
        return; // the output doesn't say which formats the primary plane reads
    }
    wlr_linux_dmabuf_v1_set_surface_feedback(frame->scene->linux_dmabuf, surface, &feedback);
    wlr_linux_dmabuf_feedback_v1_finish(&feedback);
    wlr_log(WLR_DEBUG, "%s: dmabuf feedback with the scanout tranche to a surface", frame->output->name);
}

void vela_output_frame_send_frame_done(struct vela_output_frame *frame, const struct timespec *when)
{
    for (int i = 0; i < frame->visible_count; ++i) {
        struct wlr_surface *surface = frame->visible_surfaces[i];
        const struct vela_surface_state *state = vela_surface_state_get(surface);
        if (!state || state->pacing == frame->output) {
            wlr_surface_send_frame_done(surface, when);
        }
    }
}
