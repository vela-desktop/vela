// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VELA_RENDER_CURSOR_POLICY_H
#define VELA_RENDER_CURSOR_POLICY_H

// The wlroots allocator API does not identify the purpose of a buffer.
// Never guess "cursor" from the dimensions of an ordinary output/capture
// allocation. Only a dedicated cursor swapchain may use the cursor allocator.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <wlr/render/drm_format_set.h>

// A stack-backed single-modifier format must report its array capacity to
// wlroots: wlr_swapchain_create() copies it and asserts len <= capacity.
static inline struct wlr_drm_format vela_cursor_swapchain_format(uint64_t *linear_modifier)
{
    return (struct wlr_drm_format) {
        .format = DRM_FORMAT_ARGB8888,
        .len = 1,
        .capacity = 1,
        .modifiers = linear_modifier,
    };
}

static inline bool vela_cursor_buffer_format(uint32_t format, const uint64_t *modifiers, unsigned count,
    int width, int height)
{
    return format == DRM_FORMAT_ARGB8888 && modifiers && count == 1 && modifiers[0] == DRM_FORMAT_MOD_LINEAR
        && width > 0 && height > 0 && width <= 256 && height <= 256;
}

// We only seed wlroots' cursor swapchain when a DRM output advertises one
// fixed size. With multiple sizes wlroots may choose another size later, and
// replace the swapchain with one using the ordinary allocator.
static inline bool vela_cursor_swapchain_eligible(bool drm, bool cursor_plane, size_t sizes, int width, int height,
    bool scanout_linear, bool render_linear)
{
    return drm && cursor_plane && sizes == 1 && width > 0 && height > 0 && width <= 256 && height <= 256
        && scanout_linear && render_linear;
}

static inline uint32_t vela_gbm_buffer_flags(bool cursor, bool retry)
{
    if (cursor) {
        return retry ? 0 : GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE;
    }
    return retry ? GBM_BO_USE_RENDERING : GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
}

#endif
