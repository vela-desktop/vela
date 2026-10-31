// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include "x11_coordinates.h"

#include <math.h>

bool vela_x11_scale_valid(double scale)
{
    return isfinite(scale) && scale > 0;
}

double vela_x11_global_scale(enum vela_x11_scaling_policy policy,
    const double *output_scales, size_t output_count)
{
    if (policy == VELA_X11_SCALED_BY_SYSTEM) {
        return 1.0;
    }
    double max_scale = 1.0;
    if (!output_scales) {
        return max_scale;
    }
    for (size_t i = 0; i < output_count; ++i) {
        if (vela_x11_scale_valid(output_scales[i])
            && output_scales[i] > max_scale) {
            max_scale = output_scales[i];
        }
    }
    return max_scale;
}

double vela_x11_surface_scene_scale(double global_scale,
    int x11_width, int x11_height, int surface_width, int surface_height)
{
    if (!vela_x11_scale_valid(global_scale)) {
        return 1.0;
    }
    const double fallback = 1.0 / global_scale;
    if (x11_width <= 0 || x11_height <= 0 ||
        surface_width <= 0 || surface_height <= 0) {
        return fallback;
    }
    // A fractional Wayland monitor can communicate buffer_scale 2 to
    // XWayland. Then surface->current.width may be 1280 even though
    // the X11 pixmap / geometry is 2560. Dividing by global_scale alone
    // would erroneously shrink the image a second time.
    const double x_ratio = (double)x11_width / surface_width;
    const double y_ratio = (double)x11_height / surface_height;
    if (!isfinite(x_ratio) || !isfinite(y_ratio) ||
        fabs(x_ratio - y_ratio) > 0.02 * fmax(x_ratio, y_ratio)) {
        return fallback; // async configure: don't guess anisotropic scaling
    }
    return (x_ratio + y_ratio) * 0.5 / global_scale;
}

struct vela_x11_point vela_wayland_to_x11(double global_scale, struct vela_x11_point logical)
{
    return (struct vela_x11_point) {
        logical.x * global_scale,
        logical.y * global_scale,
    };
}

struct vela_x11_point vela_x11_to_wayland(double global_scale, struct vela_x11_point x11)
{
    return (struct vela_x11_point) {
        x11.x / global_scale,
        x11.y / global_scale,
    };
}

struct vela_x11_rect vela_wayland_rect_to_x11(double scale, struct vela_x11_rect logical)
{
    return (struct vela_x11_rect) {
        logical.x * scale, logical.y * scale,
        logical.width * scale, logical.height * scale,
    };
}

struct vela_x11_rect vela_x11_rect_to_wayland(double scale, struct vela_x11_rect x11)
{
    return (struct vela_x11_rect) {
        x11.x / scale, x11.y / scale,
        x11.width / scale, x11.height / scale,
    };
}
