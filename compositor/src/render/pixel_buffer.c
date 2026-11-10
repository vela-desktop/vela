// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/pixel_buffer.h"
#include "alloc/alloc.h"

#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>

// wlr_buffer as the first member: from the pointer wlroots passes us we get
// back to the pixels.
struct pixel_buffer {
    struct wlr_buffer base;
    uint32_t *pixels;
};

static void destroy(struct wlr_buffer *buffer)
{
    struct pixel_buffer *self = (struct pixel_buffer *)buffer;
    wlr_buffer_finish(buffer);
    free(self->pixels);
    free(self);
}

static bool begin_access(struct wlr_buffer *buffer, uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
    struct pixel_buffer *self = (struct pixel_buffer *)buffer;
    *data = self->pixels;
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)buffer->width * 4;
    return true;
}

static void end_access(struct wlr_buffer *buffer)
{
}

static const struct wlr_buffer_impl pixel_buffer_impl = {
    .destroy = destroy,
    .begin_data_ptr_access = begin_access,
    .end_data_ptr_access = end_access,
};

struct wlr_buffer *vela_pixel_buffer_create(struct vela_image *image)
{
    struct pixel_buffer *buffer = VELA_NEW(struct pixel_buffer);
    buffer->pixels = image->pixels;
    if (!buffer->pixels) {
        buffer->pixels = VELA_NEW_N(uint32_t, vela_size_mul((size_t)image->width, (size_t)image->height));
    }
    wlr_buffer_init(&buffer->base, &pixel_buffer_impl, image->width, image->height);
    image->pixels = NULL;
    image->width = 0;
    image->height = 0;
    return &buffer->base;
}
