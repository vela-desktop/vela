// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Allocator of output buffers (docs/renderer.md §7.2): GBM on the render node
// of Vela's GPU, buffers exported as dmabufs with an explicit modifier, fit
// for both Vulkan drawing and scanout.

#include "render/renderer.h"

#include "util.h"

#include <drm_fourcc.h>
#include <gbm.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/util/log.h>

// wlr_allocator as the first member: wlr_allocator* <-> gbm_allocator*.
struct gbm_allocator {
    struct wlr_allocator base;
    struct gbm_device *gbm;
};

// wlr_buffer as the first member: wlr_buffer* <-> gbm_buffer*.
struct gbm_buffer {
    struct wlr_buffer base;
    struct gbm_bo *bo;
    struct wlr_dmabuf_attributes dmabuf;
};

static void buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct gbm_buffer *buffer = (struct gbm_buffer *)wlr_buffer;
    // Whoever is attached to the buffer (damage rings, the renderer's
    // caches) lets go before it disappears.
    wlr_buffer_finish(wlr_buffer);
    for (int i = 0; i < buffer->dmabuf.n_planes; ++i) {
        close(buffer->dmabuf.fd[i]);
    }
    gbm_bo_destroy(buffer->bo);
    free(buffer);
}

static bool buffer_get_dmabuf(struct wlr_buffer *wlr_buffer, struct wlr_dmabuf_attributes *attribs)
{
    *attribs = ((struct gbm_buffer *)wlr_buffer)->dmabuf;
    return true;
}

static const struct wlr_buffer_impl buffer_impl = {
    .destroy = buffer_destroy,
    .get_dmabuf = buffer_get_dmabuf,
};

static struct wlr_buffer *create_buffer(struct wlr_allocator *wlr_alloc, int width, int height,
    const struct wlr_drm_format *format)
{
    struct gbm_allocator *alloc = (struct gbm_allocator *)wlr_alloc;

    // Explicit modifiers only: Vulkan must know how the buffer is laid out.
    uint64_t *modifiers = calloc(format->len ? format->len : 1, sizeof(*modifiers));
    unsigned count = 0;
    bool has_linear = false;
    for (size_t i = 0; i < format->len; ++i) {
        if (format->modifiers[i] != DRM_FORMAT_MOD_INVALID) {
            modifiers[count++] = format->modifiers[i];
            has_linear = has_linear || format->modifiers[i] == DRM_FORMAT_MOD_LINEAR;
        }
    }
    if (count == 0) {
        wlr_log(WLR_ERROR, "Allocator: no explicit modifier for format 0x%08x", format->format);
        free(modifiers);
        return NULL;
    }
    // Diagnostics: VELA_DEBUG_LINEAR=1 uses linear buffers, without GPU tiling
    // or compression (to rule out compatibility problems).
    if (has_linear && vela_env_one("VELA_DEBUG_LINEAR")) {
        modifiers[0] = DRM_FORMAT_MOD_LINEAR;
        count = 1;
    }

    struct gbm_bo *bo = gbm_bo_create_with_modifiers2(alloc->gbm, (uint32_t)width, (uint32_t)height, format->format,
        modifiers, count, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!bo) {
        // Some drivers refuse SCANOUT on certain modifiers (such as headless).
        bo = gbm_bo_create_with_modifiers2(alloc->gbm, (uint32_t)width, (uint32_t)height, format->format, modifiers,
            count, GBM_BO_USE_RENDERING);
    }
    free(modifiers);
    if (!bo) {
        wlr_log_errno(WLR_ERROR, "Allocator: gbm_bo_create_with_modifiers2 %dx%d", width, height);
        return NULL;
    }

    struct gbm_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        gbm_bo_destroy(bo);
        return NULL;
    }
    buffer->bo = bo;
    struct wlr_dmabuf_attributes *d = &buffer->dmabuf;
    d->width = width;
    d->height = height;
    d->format = format->format;
    d->modifier = gbm_bo_get_modifier(bo);
    d->n_planes = gbm_bo_get_plane_count(bo);
    if (d->n_planes < 1 || d->n_planes > WLR_DMABUF_MAX_PLANES) {
        wlr_log(WLR_ERROR, "Allocator: invalid GBM plane count %d", d->n_planes);
        gbm_bo_destroy(bo);
        free(buffer);
        return NULL;
    }
    for (int i = 0; i < d->n_planes; ++i) {
        d->fd[i] = gbm_bo_get_fd_for_plane(bo, i);
        d->offset[i] = gbm_bo_get_offset(bo, i);
        d->stride[i] = gbm_bo_get_stride_for_plane(bo, i);
        if (d->fd[i] < 0) {
            wlr_log_errno(WLR_ERROR, "Allocator: exporting plane %d", i);
            for (int j = 0; j < i; ++j) {
                close(d->fd[j]);
            }
            gbm_bo_destroy(bo);
            free(buffer);
            return NULL;
        }
    }
    wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
    return &buffer->base;
}

static void allocator_destroy(struct wlr_allocator *wlr_alloc)
{
    struct gbm_allocator *alloc = (struct gbm_allocator *)wlr_alloc;
    gbm_device_destroy(alloc->gbm);
    free(alloc);
}

static const struct wlr_allocator_interface allocator_impl = {
    .create_buffer = create_buffer,
    .destroy = allocator_destroy,
};

struct wlr_allocator *vela_gbm_allocator_create(int render_fd)
{
    struct gbm_device *gbm = gbm_create_device(render_fd);
    if (!gbm) {
        wlr_log(WLR_ERROR, "Allocator: can't create the GBM device");
        return NULL;
    }
    struct gbm_allocator *alloc = calloc(1, sizeof(*alloc));
    alloc->gbm = gbm;
    wlr_allocator_init(&alloc->base, &allocator_impl, WLR_BUFFER_CAP_DMABUF);
    return &alloc->base;
}
