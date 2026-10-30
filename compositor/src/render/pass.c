// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// A drawing on a buffer: quads are collected (textures, solid colors, shadows,
// blurs), then vela_pass_submit records a single command buffer and submits it
// to the GPU, with implicit sync toward whoever uses the same buffers (kernel,
// apps, host compositor) and explicit sync with the apps that ask for it. To
// wlroots it's also a wlr_render_pass.

#include "render/render.h"

#include "color.h"
#include "util.h"

#include <inttypes.h>
#include <linux/dma-buf.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/util/log.h>

// A quad ready for the GPU: one piece per clip rectangle.
struct draw {
    enum vela_pipeline_kind kind;
    bool blend;
    bool linear;
    struct vela_texture *texture; // NULL: rectangle or shadow
    struct vela_quad_push push;
    VkRect2D scissor;
    int blur_op; // before drawing it, the blur blur_ops[blur_op]; -1: none
};

// A blur to compute: the output zone to read.
struct blur_op {
    struct wlr_box source;
    float strength;
};

// A point on an app's timeline to wait for before reading its buffer (explicit
// sync). The timeline reference is ours.
struct sync_wait {
    struct vela_texture *texture;
    struct wlr_drm_syncobj_timeline *timeline;
    uint64_t point;
};

struct vela_pass {
    struct wlr_render_pass base; // first member: wlr_render_pass* <-> vela_pass*
    struct vela_renderer *renderer;
    struct vela_target *target;
    struct wlr_buffer *buffer; // locked as long as the pass exists

    struct draw *draws;
    int draw_count, draw_capacity;
    struct blur_op *blur_ops;
    int blur_count, blur_capacity;
    struct sync_wait *waits;
    int wait_count, wait_capacity;

    struct wlr_drm_syncobj_timeline *signal_timeline; // our reference
    uint64_t signal_point;
    int timing_slot;
    bool filtered;
    float filter[12];
};

static const struct wlr_render_pass_impl pass_impl;

// The wl_output_transform matrices (as in wlroots), rows [a b; d e]: the
// texture rotated/flipped in the unit square.
struct rotation {
    float a, b, d, e;
};

static struct rotation rotation_of(enum wl_output_transform transform)
{
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_NORMAL: return (struct rotation) { 1, 0, 0, 1 };
    case WL_OUTPUT_TRANSFORM_90: return (struct rotation) { 0, 1, -1, 0 };
    case WL_OUTPUT_TRANSFORM_180: return (struct rotation) { -1, 0, 0, -1 };
    case WL_OUTPUT_TRANSFORM_270: return (struct rotation) { 0, -1, 1, 0 };
    case WL_OUTPUT_TRANSFORM_FLIPPED: return (struct rotation) { -1, 0, 0, 1 };
    case WL_OUTPUT_TRANSFORM_FLIPPED_90: return (struct rotation) { 0, 1, 1, 0 };
    case WL_OUTPUT_TRANSFORM_FLIPPED_180: return (struct rotation) { 1, 0, 0, -1 };
    case WL_OUTPUT_TRANSFORM_FLIPPED_270: return (struct rotation) { 0, -1, -1, 0 };
    }
    return (struct rotation) { 1, 0, 0, 1 };
}

// From premultiplied sRGB (as wlroots) to premultiplied linear.
static void linear_color(const struct wlr_render_color *color, float min_alpha, float out[4])
{
    float a = vela_clampf(color->a, min_alpha, 1.0f);
    const float channels[3] = { color->r, color->g, color->b };
    for (int i = 0; i < 3; ++i) {
        float straight = a > 0.0f ? vela_clampf(channels[i] / a, 0.0f, 1.0f) : 0.0f;
        out[i] = vela_srgb_to_linear(straight) * a;
    }
    out[3] = a;
}

static void set_box(float out[4], const struct wlr_box *box)
{
    out[0] = (float)box->x;
    out[1] = (float)box->y;
    out[2] = (float)box->width;
    out[3] = (float)box->height;
}

// ---------------------------------------------------------------- creation --

struct vela_pass *vela_renderer_begin_pass(struct vela_renderer *renderer, struct wlr_buffer *buffer)
{
    struct vela_target *target = vela_renderer_target(renderer, buffer);
    if (!target) {
        return NULL;
    }
    struct vela_pass *pass = calloc(1, sizeof(*pass));
    if (!pass) {
        wlr_log_errno(WLR_ERROR, "Renderer: allocating render pass");
        return NULL;
    }
    wlr_render_pass_init(&pass->base, &pass_impl);
    pass->renderer = renderer;
    pass->target = target;
    pass->buffer = wlr_buffer_lock(buffer);
    pass->timing_slot = -1;
    return pass;
}

static void pass_free(struct vela_pass *pass)
{
    for (int i = 0; i < pass->wait_count; ++i) {
        wlr_drm_syncobj_timeline_unref(pass->waits[i].timeline);
    }
    if (pass->signal_timeline) {
        wlr_drm_syncobj_timeline_unref(pass->signal_timeline);
    }
    wlr_buffer_unlock(pass->buffer);
    free(pass->draws);
    free(pass->blur_ops);
    free(pass->waits);
    free(pass);
}

struct wlr_render_pass *vela_pass_wlr(struct vela_pass *pass)
{
    return &pass->base;
}

void vela_pass_measure(struct vela_pass *pass)
{
    if (pass->timing_slot < 0) {
        pass->timing_slot = vela_renderer_timing_slot(pass->renderer);
    }
}

void vela_pass_signal_on_done(struct vela_pass *pass, struct wlr_drm_syncobj_timeline *timeline, uint64_t point)
{
    if (pass->signal_timeline) {
        wlr_drm_syncobj_timeline_unref(pass->signal_timeline);
    }
    pass->signal_timeline = wlr_drm_syncobj_timeline_ref(timeline);
    pass->signal_point = point;
}

void vela_pass_set_color_filter(struct vela_pass *pass, const float *matrix)
{
    pass->filtered = matrix != NULL;
    if (!matrix) {
        return;
    }
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            pass->filter[row * 4 + column] = matrix[row * 3 + column];
        }
        pass->filter[row * 4 + 3] = 0.0f;
    }
}

// ---------------------------------------------------------------- quads --

// One draw per clip rectangle, with the scissor.
static void add_draw(struct vela_pass *pass, const struct draw *draw, const struct wlr_box *dst,
    const pixman_region32_t *clip)
{
    pixman_region32_t region;
    pixman_region32_init_rect(&region, dst->x, dst->y, (unsigned)(dst->width > 0 ? dst->width : 0),
        (unsigned)(dst->height > 0 ? dst->height : 0));
    pixman_region32_intersect_rect(&region, &region, 0, 0, pass->target->width, pass->target->height);
    if (clip) {
        pixman_region32_intersect(&region, &region, clip);
    }
    int count = 0;
    const pixman_box32_t *rects = pixman_region32_rectangles(&region, &count);
    pass->draws = vela_grow(pass->draws, &pass->draw_capacity, pass->draw_count + count, sizeof(*pass->draws));
    for (int i = 0; i < count; ++i) {
        struct draw *piece = &pass->draws[pass->draw_count++];
        *piece = *draw;
        piece->scissor = (VkRect2D) {
            { rects[i].x1, rects[i].y1 },
            { (uint32_t)(rects[i].x2 - rects[i].x1), (uint32_t)(rects[i].y2 - rects[i].y1) },
        };
    }
    pixman_region32_fini(&region);
}

// A texture's quad, before the clip pieces. false if there is nothing to draw.
static bool prepare_texture(struct vela_pass *pass, const struct vela_texture_draw *in, struct draw *draw)
{
    struct vela_texture *texture = vela_texture_from_wlr_texture(in->texture);
    if (!texture || !texture->view || in->dst.width <= 0 || in->dst.height <= 0 || in->alpha <= 0.0f) {
        return false;
    }
    if (in->wait_timeline) {
        bool known = false;
        for (int i = 0; i < pass->wait_count && !known; ++i) {
            const struct sync_wait *wait = &pass->waits[i];
            known = wait->texture == texture && wait->timeline == in->wait_timeline && wait->point == in->wait_point;
        }
        if (!known) {
            pass->waits = vela_grow(pass->waits, &pass->wait_capacity, pass->wait_count + 1, sizeof(*pass->waits));
            pass->waits[pass->wait_count++]
                = (struct sync_wait) { texture, wlr_drm_syncobj_timeline_ref(in->wait_timeline), in->wait_point };
        }
    }
    float tex_width = (float)texture->base.width;
    float tex_height = (float)texture->base.height;
    struct wlr_fbox src = in->src;
    if (wlr_fbox_empty(&src)) {
        src = (struct wlr_fbox) { 0, 0, tex_width, tex_height };
    }

    memset(draw, 0, sizeof(*draw));
    draw->kind = VELA_PIPELINE_TEXTURE;
    draw->texture = texture;
    draw->linear = in->linear;
    draw->blur_op = -1;
    struct vela_quad_push *p = &draw->push;
    // A real magnification (neither a 1:1 copy nor a reduction): bicubic
    // filter instead of bilinear, which softens.
    bool swapped = in->transform & WL_OUTPUT_TRANSFORM_90;
    double shown_width = swapped ? in->dst.height : in->dst.width;
    double shown_height = swapped ? in->dst.width : in->dst.height;
    if (in->linear && shown_width > src.width * 1.01 && shown_height > src.height * 1.01) {
        p->flags |= 1u;
    }
    // An opaque quad doesn't need blending with what is below (clipped at the
    // corners it does).
    bool shaped = in->shape_radius > 0.0f && !wlr_box_empty(&in->shape_rect);
    draw->blend = in->blend && (texture->format->alpha || in->alpha < 1.0f || shaped);
    if (shaped) {
        p->flags |= 2u;
        set_box(p->shape_rect, &in->shape_rect);
        p->shape[0] = in->shape_radius;
    }
    set_box(p->dst, &in->dst);
    p->target[0] = (float)pass->target->width;
    p->target[1] = (float)pass->target->height;
    p->alpha = in->alpha;

    // From the quad's corner (u, v in [0,1]) to texture coordinates: undo the
    // transformation in the unit square (an orthogonal matrix: the inverse is
    // the transpose), then go into the src zone.
    struct rotation r = rotation_of(in->transform);
    float corners[3][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 } }; // origin, right, down
    float uv[3][2];
    for (int i = 0; i < 3; ++i) {
        float px = 2.0f * corners[i][0] - 1.0f;
        float py = 2.0f * corners[i][1] - 1.0f;
        float qx = r.a * px + r.d * py;
        float qy = r.b * px + r.e * py;
        float s = (qx + 1.0f) / 2.0f;
        float t = (qy + 1.0f) / 2.0f;
        uv[i][0] = (float)((src.x + s * src.width) / tex_width);
        uv[i][1] = (float)((src.y + t * src.height) / tex_height);
    }
    p->uv_origin[0] = uv[0][0];
    p->uv_origin[1] = uv[0][1];
    p->uv_x[0] = uv[1][0] - uv[0][0];
    p->uv_x[1] = uv[1][1] - uv[0][1];
    p->uv_y[0] = uv[2][0] - uv[0][0];
    p->uv_y[1] = uv[2][1] - uv[0][1];
    return true;
}

void vela_pass_add_texture(struct vela_pass *pass, const struct vela_texture_draw *in)
{
    struct draw draw;
    if (prepare_texture(pass, in, &draw)) {
        add_draw(pass, &draw, &in->dst, in->clip);
    }
}

void vela_pass_add_rect(struct vela_pass *pass, const struct wlr_box *box, const struct wlr_render_color *color,
    const pixman_region32_t *clip, bool blend, const struct wlr_box *shape_rect, float shape_radius)
{
    if (box->width <= 0 || box->height <= 0) {
        return;
    }
    struct draw draw = { .kind = VELA_PIPELINE_RECT, .blur_op = -1 };
    bool shaped = shape_radius > 0.0f && shape_rect && !wlr_box_empty(shape_rect);
    draw.blend = blend && (color->a < 1.0f || shaped);
    struct vela_quad_push *p = &draw.push;
    if (shaped) {
        p->flags |= 2u;
        set_box(p->shape_rect, shape_rect);
        p->shape[0] = shape_radius;
    }
    set_box(p->dst, box);
    p->target[0] = (float)pass->target->width;
    p->target[1] = (float)pass->target->height;
    p->alpha = 1.0f;
    linear_color(color, 0.0f, p->color);
    add_draw(pass, &draw, box, clip);
}

void vela_pass_add_shadow(struct vela_pass *pass, const struct wlr_box *box, const struct wlr_box *caster,
    const struct wlr_box *window, float radius, float sigma, const struct wlr_render_color *color,
    const pixman_region32_t *clip)
{
    if (box->width <= 0 || box->height <= 0 || color->a <= 0.0f) {
        return;
    }
    struct draw draw = { .kind = VELA_PIPELINE_SHADOW, .blend = true, .blur_op = -1 };
    struct vela_quad_push *p = &draw.push;
    set_box(p->dst, box);
    p->target[0] = (float)pass->target->width;
    p->target[1] = (float)pass->target->height;
    p->alpha = 1.0f;
    linear_color(color, 0.0f, p->color);
    set_box(p->shape_rect, caster);
    p->shape[0] = radius;
    p->shape[1] = sigma;
    // The window, where the shadow doesn't go: in the texture fields, unused
    // here.
    p->uv_origin[0] = (float)window->x;
    p->uv_origin[1] = (float)window->y;
    p->uv_x[0] = (float)window->width;
    p->uv_x[1] = (float)window->height;
    add_draw(pass, &draw, box, clip);
}

// ----------------------------------------------------------------- blur --

int vela_blur_reach(float strength)
{
    // Each level widens the reach by about two of its pixels, which are
    // 2^level output pixels: the sum, with some margin.
    return (int)ceilf(strength * 3.0f * (float)(1 << VELA_BLUR_LEVELS)) + 2;
}

void vela_pass_add_blur(struct vela_pass *pass, const struct vela_texture_draw *panel,
    const pixman_region32_t *region, float strength, const struct wlr_render_color *tint)
{
    if (!pass->target->sampleable || !region || !pixman_region32_not_empty(region)) {
        return;
    }
    const pixman_box32_t *extents = pixman_region32_extents(region);
    int reach = vela_blur_reach(strength);
    struct wlr_box source = {
        extents->x1 - reach,
        extents->y1 - reach,
        extents->x2 - extents->x1 + 2 * reach,
        extents->y2 - extents->y1 + 2 * reach,
    };
    const struct wlr_box whole = { 0, 0, (int)pass->target->width, (int)pass->target->height };
    if (!wlr_box_intersection(&source, &source, &whole)
        || !vela_renderer_prepare_blur(pass->renderer, (uint32_t)source.width, (uint32_t)source.height)) {
        return;
    }

    struct draw draw;
    if (!prepare_texture(pass, panel, &draw)) {
        return;
    }
    draw.kind = VELA_PIPELINE_BLUR_MIX;
    draw.blend = true;
    draw.linear = true;
    draw.push.flags &= ~1u; // the panel is read only for its alpha
    if (tint) {
        draw.push.flags |= 8u; // Vela's acrylic: more saturation, and the tint
    }
    draw.blur_op = pass->blur_count;
    pass->blur_ops = vela_grow(pass->blur_ops, &pass->blur_capacity, pass->blur_count + 1, sizeof(*pass->blur_ops));
    pass->blur_ops[pass->blur_count++] = (struct blur_op) { source, strength };
    // From the output pixel to level 0 (half resolution) of the work images.
    const struct vela_blur_image *level0 = &pass->renderer->blur[0];
    draw.push.pad[0] = (float)source.x;
    draw.push.pad[1] = (float)source.y;
    draw.push.shape[2] = 0.5f / (float)level0->width;
    draw.push.shape[3] = 0.5f / (float)level0->height;
    const struct wlr_render_color none = { 0 };
    linear_color(tint ? tint : &none, tint ? 0.001f : 0.0f, draw.push.color);
    add_draw(pass, &draw, &panel->dst, region);
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
    VkAccessFlags2 dst_access)
{
    const VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

#define DRAW_STAGES (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
#define DRAW_ACCESS (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT)

// One blur pass: from `source` (zone `src` in a texture of src_width x
// src_height) to level `level`, used for used_width x used_height.
static void blur_step(struct vela_pass *pass, VkCommandBuffer cmd, enum vela_pipeline_kind kind, VkImageView source,
    const struct wlr_box *src, uint32_t src_width, uint32_t src_height, int level, uint32_t used_width,
    uint32_t used_height, float strength)
{
    struct vela_renderer *renderer = pass->renderer;
    const struct vela_blur_image *dst = &renderer->blur[level];
    VkPipelineLayout layout = renderer->layout;
    barrier(cmd, dst->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, DRAW_STAGES,
        DRAW_ACCESS, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    const VkRenderingAttachmentInfo attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = dst->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { used_width, used_height } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    vkCmdBeginRendering(cmd, &rendering);
    const VkViewport viewport = { 0.0f, 0.0f, (float)dst->width, (float)dst->height, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkRect2D scissor = { { 0, 0 }, { used_width, used_height } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        vela_renderer_pipeline(renderer, VELA_BLUR_FORMAT, kind, false));
    const VkDescriptorImageInfo image = {
        .sampler = renderer->linear,
        .imageView = source,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image,
    };
    vkCmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &write);
    struct vela_quad_push p = { 0 };
    p.dst[2] = (float)used_width;
    p.dst[3] = (float)used_height;
    p.target[0] = (float)dst->width;
    p.target[1] = (float)dst->height;
    p.alpha = 1.0f;
    p.uv_origin[0] = (float)src->x / (float)src_width;
    p.uv_origin[1] = (float)src->y / (float)src_height;
    p.uv_x[0] = (float)src->width / (float)src_width;
    p.uv_y[1] = (float)src->height / (float)src_height;
    // Half a source texel, for the reach; and where it can read.
    p.shape[0] = 0.5f / (float)src_width * strength;
    p.shape[1] = 0.5f / (float)src_height * strength;
    p.shape_rect[0] = ((float)src->x + 0.5f) / (float)src_width;
    p.shape_rect[1] = ((float)src->y + 0.5f) / (float)src_height;
    p.shape_rect[2] = ((float)(src->x + src->width) - 0.5f) / (float)src_width;
    p.shape_rect[3] = ((float)(src->y + src->height) - 0.5f) / (float)src_height;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 4, 1, 0, 0);
    vkCmdEndRendering(cmd);
    barrier(cmd, dst->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

static void run_blur(struct vela_pass *pass, VkCommandBuffer cmd, const struct blur_op *op)
{
    struct vela_target *target = pass->target;
    // The output drawn so far becomes readable.
    vkCmdEndRendering(cmd);
    barrier(cmd, target->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // The used size of each level.
    uint32_t used_width[VELA_BLUR_LEVELS];
    uint32_t used_height[VELA_BLUR_LEVELS];
    for (int level = 0; level < VELA_BLUR_LEVELS; ++level) {
        used_width[level] = ((uint32_t)op->source.width + (2u << level) - 1) >> (level + 1);
        used_height[level] = ((uint32_t)op->source.height + (2u << level) - 1) >> (level + 1);
        used_width[level] = used_width[level] ? used_width[level] : 1;
        used_height[level] = used_height[level] ? used_height[level] : 1;
    }
    // Down: from the output to level 0, then each level from the previous one.
    blur_step(pass, cmd, VELA_PIPELINE_BLUR_DOWN, target->view, &op->source, target->width, target->height, 0,
        used_width[0], used_height[0], op->strength);
    for (int level = 1; level < VELA_BLUR_LEVELS; ++level) {
        const struct vela_blur_image *from = &pass->renderer->blur[level - 1];
        const struct wlr_box zone = { 0, 0, (int)used_width[level - 1], (int)used_height[level - 1] };
        blur_step(pass, cmd, VELA_PIPELINE_BLUR_DOWN, from->view, &zone, from->width, from->height, level,
            used_width[level], used_height[level], op->strength);
    }
    // Up: back to level 0.
    for (int level = VELA_BLUR_LEVELS - 2; level >= 0; --level) {
        const struct vela_blur_image *from = &pass->renderer->blur[level + 1];
        const struct wlr_box zone = { 0, 0, (int)used_width[level + 1], (int)used_height[level + 1] };
        blur_step(pass, cmd, VELA_PIPELINE_BLUR_UP, from->view, &zone, from->width, from->height, level,
            used_width[level], used_height[level], op->strength);
    }

    // Back to drawing on the output.
    barrier(cmd, target->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

// ----------------------------------------------------------- submission --

// The texture is read with explicit sync (a point to wait for).
static bool explicit_sync(const struct vela_pass *pass, const struct vela_texture *texture)
{
    for (int i = 0; i < pass->wait_count; ++i) {
        if (pass->waits[i].texture == texture) {
            return true;
        }
    }
    return false;
}

// The imported textures (dmabuf) of this drawing, once each, in an allocated
// array.
static struct vela_texture **foreign_textures(const struct vela_pass *pass, int *count)
{
    struct vela_texture **foreign = calloc((size_t)(pass->draw_count ? pass->draw_count : 1), sizeof(*foreign));
    *count = 0;
    for (int i = 0; i < pass->draw_count; ++i) {
        struct vela_texture *texture = pass->draws[i].texture;
        if (!texture || !texture->foreign) {
            continue;
        }
        bool seen = false;
        for (int j = 0; j < *count && !seen; ++j) {
            seen = foreign[j] == texture;
        }
        if (!seen) {
            foreign[(*count)++] = texture;
        }
    }
    return foreign;
}

// Acquire and release of an image the "foreign" queue (kernel, other GPUs,
// other processes) shares with us.
static VkImageMemoryBarrier2 ownership(VkImage image, uint32_t queue_family, bool acquire, VkImageLayout layout,
    VkPipelineStageFlags2 stage, VkAccessFlags2 access, VkImageLayout rest)
{
    return (VkImageMemoryBarrier2) {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = acquire ? VK_PIPELINE_STAGE_2_NONE : stage,
        .srcAccessMask = acquire ? VK_ACCESS_2_NONE : access,
        .dstStageMask = acquire ? stage : VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask = acquire ? access : VK_ACCESS_2_NONE,
        .oldLayout = acquire ? rest : layout,
        .newLayout = acquire ? layout : VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_FOREIGN_EXT : queue_family,
        .dstQueueFamilyIndex = acquire ? queue_family : VK_QUEUE_FAMILY_FOREIGN_EXT,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
}

// The drawing's commands, from the first barrier to the last.
static void record(struct vela_pass *pass, VkCommandBuffer cmd, struct vela_texture **foreign, int foreign_count)
{
    struct vela_renderer *renderer = pass->renderer;
    struct vela_target *target = pass->target;
    uint32_t family = renderer->vk->queue_family;
    VkPipelineLayout layout = renderer->layout;

    VkImageMemoryBarrier2 *acquire = calloc((size_t)foreign_count + 1, sizeof(*acquire));
    VkImageMemoryBarrier2 *release = calloc((size_t)foreign_count + 1, sizeof(*release));
    // The target's content is kept: only what changed is redrawn.
    acquire[0] = ownership(target->image, family, true, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        target->initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
    release[0] = ownership(target->image, family, false, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_GENERAL);
    for (int i = 0; i < foreign_count; ++i) {
        acquire[i + 1] = ownership(foreign[i]->image, family, true, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
        release[i + 1] = ownership(foreign[i]->image, family, false, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_GENERAL);
    }
    const VkDependencyInfo acquire_dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = (uint32_t)foreign_count + 1,
        .pImageMemoryBarriers = acquire,
    };
    vkCmdPipelineBarrier2(cmd, &acquire_dependency);

    const VkRenderingAttachmentInfo attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { target->width, target->height } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    vkCmdBeginRendering(cmd, &rendering);
    const VkViewport viewport = { 0.0f, 0.0f, (float)target->width, (float)target->height, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkPipeline bound = VK_NULL_HANDLE;
    int blur_done = -1;
    for (int i = 0; i < pass->draw_count; ++i) {
        const struct draw *draw = &pass->draws[i];
        // A blur: what is behind is read before its pieces.
        if (draw->blur_op >= 0 && draw->blur_op != blur_done) {
            run_blur(pass, cmd, &pass->blur_ops[draw->blur_op]);
            blur_done = draw->blur_op;
            vkCmdBeginRendering(cmd, &rendering);
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            bound = VK_NULL_HANDLE;
        }
        VkPipeline pipeline = vela_renderer_pipeline(renderer, target->format, draw->kind, draw->blend);
        if (!pipeline) {
            continue;
        }
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }
        if (draw->texture) {
            const VkDescriptorImageInfo image = {
                .sampler = draw->linear ? renderer->linear : renderer->nearest,
                .imageView = draw->texture->view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            const VkDescriptorImageInfo blurred = {
                .sampler = renderer->linear,
                .imageView = renderer->blur[0].view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            const VkWriteDescriptorSet writes[] = {
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &image,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 1,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &blurred,
                },
            };
            // The blur composition also reads the blurred background.
            uint32_t count = draw->kind == VELA_PIPELINE_BLUR_MIX ? 2 : 1;
            vkCmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, count, writes);
        }
        struct vela_quad_push push = draw->push;
        if (pass->filtered) {
            push.flags |= 4u;
            memcpy(push.filter, pass->filter, sizeof(push.filter));
        }
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
            &push);
        vkCmdSetScissor(cmd, 0, 1, &draw->scissor);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);

    const VkDependencyInfo release_dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = (uint32_t)foreign_count + 1,
        .pImageMemoryBarriers = release,
    };
    vkCmdPipelineBarrier2(cmd, &release_dependency);
    free(acquire);
    free(release);
}

// A dmabuf's implicit fence (sync_file), to add to the waits.
static void export_fence(int dmabuf_fd, uint32_t flags, int *fds, int *count)
{
    struct dma_buf_export_sync_file request = { .flags = flags, .fd = -1 };
    if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request) == 0 && request.fd >= 0) {
        fds[(*count)++] = request.fd;
    }
}

static bool import_fence(int dmabuf_fd, uint32_t flags, int sync_file)
{
    struct dma_buf_import_sync_file request = { .flags = flags, .fd = sync_file };
    return ioctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &request) == 0;
}

bool vela_pass_submit(struct vela_pass *pass, struct vela_pass_result *result)
{
    struct vela_renderer *renderer = pass->renderer;
    if (renderer->vk->loss.lost) {
        if (pass->signal_timeline) {
            wlr_drm_syncobj_timeline_signal(pass->signal_timeline, pass->signal_point);
        }
        pass_free(pass);
        return false;
    }
    struct vela_target *target = pass->target;
    if (result) {
        *result = (struct vela_pass_result) { 0, -1, 0 };
    }
    vela_renderer_collect(renderer);

    VkCommandBuffer cmd = vela_renderer_begin_commands(renderer);
    if (!cmd) {
        if (pass->signal_timeline) {
            // Whoever waits for this point must not stay blocked.
            wlr_drm_syncobj_timeline_signal(pass->signal_timeline, pass->signal_point);
        }
        vela_renderer_timing_submitted(renderer, pass->timing_slot, 0);
        pass_free(pass);
        return false;
    }
    int foreign_count = 0;
    struct vela_texture **foreign = foreign_textures(pass, &foreign_count);
    vela_renderer_write_timestamp(renderer, cmd, pass->timing_slot, false);
    record(pass, cmd, foreign, foreign_count);
    vela_renderer_write_timestamp(renderer, cmd, pass->timing_slot, true);

    // The waits: one per explicit sync point, one for the target and one per
    // imported texture.
    int *waits = calloc((size_t)(pass->wait_count + foreign_count + 1), sizeof(*waits));
    int wait_count = 0;
    // Explicit sync: apps with linux-drm-syncobj-v1 say themselves which point
    // to wait for before reading the buffer.
    for (int i = 0; i < pass->wait_count; ++i) {
        int fd = wlr_drm_syncobj_timeline_export_sync_file(pass->waits[i].timeline, pass->waits[i].point);
        if (fd >= 0) {
            waits[wait_count++] = fd;
        } else {
            wlr_log(WLR_ERROR, "Pass: can't wait for point %" PRIu64 " of an app", pass->waits[i].point);
        }
    }
    // Implicit sync: wait for whoever still uses the target (the output
    // showing it) and whoever is still writing the textures (the apps); at the
    // end our fence goes into the same dmabufs.
    // CPU synchronization still needs the producers' fences. Only the Vulkan
    // import/export is disabled; ignoring implicit fences would race clients.
    export_fence(target->dmabuf_fd, DMA_BUF_SYNC_WRITE, waits, &wait_count);
    for (int i = 0; i < foreign_count; ++i) {
        if (!explicit_sync(pass, foreign[i])) {
            export_fence(foreign[i]->dmabuf_fd, DMA_BUF_SYNC_READ, waits, &wait_count);
        }
    }

    int release_fd = -1;
    uint64_t point = vela_renderer_submit(renderer, cmd, waits, wait_count, &release_fd);
    free(waits);
    vela_renderer_timing_submitted(renderer, pass->timing_slot, point);
    if (point == 0) {
        if (pass->signal_timeline) {
            wlr_drm_syncobj_timeline_signal(pass->signal_timeline, pass->signal_point);
        }
        free(foreign);
        pass_free(pass);
        return false;
    }
    if (release_fd >= 0) {
        bool ok = import_fence(target->dmabuf_fd, DMA_BUF_SYNC_WRITE, release_fd);
        for (int i = 0; i < foreign_count; ++i) {
            if (!explicit_sync(pass, foreign[i])) {
                ok = import_fence(foreign[i]->dmabuf_fd, DMA_BUF_SYNC_READ, release_fd) && ok;
            }
        }
        if (!ok) {
            vela_renderer_wait(renderer, point); // the kernel didn't take the fence
        }
    }
    free(foreign);
    // End of work on the syncobj timelines too: ours (release of app buffers)
    // and the one wlroots asked for. release_fd -1: the CPU has already
    // waited, the points signal at once.
    uint64_t sync_point = vela_renderer_signal_sync_point(renderer, release_fd);
    if (pass->signal_timeline) {
        bool signalled = release_fd >= 0
            ? wlr_drm_syncobj_timeline_import_sync_file(pass->signal_timeline, pass->signal_point, release_fd)
            : wlr_drm_syncobj_timeline_signal(pass->signal_timeline, pass->signal_point);
        if (!signalled) {
            vela_renderer_wait(renderer, point);
            wlr_drm_syncobj_timeline_signal(pass->signal_timeline, pass->signal_point);
        }
    }
    if (release_fd >= 0) {
        close(release_fd);
    }
    if (renderer->vk->loss.lost) {
        pass_free(pass);
        return false;
    }
    target->initialized = true;
    if (result) {
        *result = (struct vela_pass_result) { point, pass->timing_slot, sync_point };
    }
    pass_free(pass);
    return true;
}

// ------------------------------------------------------ wlr_render_pass --

static bool wlr_pass_submit(struct wlr_render_pass *wlr)
{
    return vela_pass_submit((struct vela_pass *)wlr, NULL); // wlroots won't use it anymore
}

static void wlr_pass_add_texture(struct wlr_render_pass *wlr, const struct wlr_render_texture_options *options)
{
    struct vela_texture_draw draw = { .texture = options->texture };
    wlr_render_texture_options_get_src_box(options, &draw.src);
    wlr_render_texture_options_get_dst_box(options, &draw.dst);
    draw.transform = options->transform;
    draw.alpha = wlr_render_texture_options_get_alpha(options);
    draw.linear = options->filter_mode == WLR_SCALE_FILTER_BILINEAR;
    draw.blend = options->blend_mode == WLR_RENDER_BLEND_MODE_PREMULTIPLIED;
    draw.clip = options->clip;
    draw.wait_timeline = options->wait_timeline;
    draw.wait_point = options->wait_point;
    vela_pass_add_texture((struct vela_pass *)wlr, &draw);
}

static void wlr_pass_add_rect(struct wlr_render_pass *wlr, const struct wlr_render_rect_options *options)
{
    struct vela_pass *pass = (struct vela_pass *)wlr;
    struct wlr_box box = options->box;
    if (wlr_box_empty(&box)) {
        box = (struct wlr_box) { 0, 0, (int)pass->target->width, (int)pass->target->height };
    }
    vela_pass_add_rect(pass, &box, &options->color, options->clip,
        options->blend_mode == WLR_RENDER_BLEND_MODE_PREMULTIPLIED, NULL, 0.0f);
}

static const struct wlr_render_pass_impl pass_impl = {
    .submit = wlr_pass_submit,
    .add_texture = wlr_pass_add_texture,
    .add_rect = wlr_pass_add_rect,
};
