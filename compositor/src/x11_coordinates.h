// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VELA_X11_COORDINATES_H
#define VELA_X11_COORDINATES_H

#include <stdbool.h>
#include <stddef.h>

// KWin-style model: there is ONE XWayland coordinate scale per X server,
// not one independent X11 scale per monitor. In "applications scale themselves"
// mode use the largest enabled output's scale. Every output is transformed
// with the SAME factor so its XRandR rectangles remain contiguous.
enum vela_x11_scaling_policy {
    VELA_X11_SCALED_BY_SYSTEM,
    VELA_X11_APPLY_SCALING_THEMSELVES,
};

struct vela_x11_point {
    double x, y;
};

struct vela_x11_rect {
    double x, y, width, height;
};

// Values must be finite, positive; invalid entries are ignored. If all output
// values are missing/invalid, use 1.0 for a headless or startup transition.
double vela_x11_global_scale(enum vela_x11_scaling_policy policy,
    const double *output_scales, size_t output_count);

bool vela_x11_scale_valid(double scale);
// Convert XWayland's (possibly pre-scaled via wl_surface.buffer_scale)
// surface units into Vela scene units. X11 window sizes are X11 pixels;
// the committed Wayland surface may be smaller after wl_buffer scaling.
// A consistent width/height ratio is required to avoid resizing a surface
// against stale X11 configure dimensions.
double vela_x11_surface_scene_scale(double global_scale,
    int x11_width, int x11_height, int surface_width, int surface_height);

struct vela_x11_point vela_wayland_to_x11(double global_scale, struct vela_x11_point logical);
struct vela_x11_point vela_x11_to_wayland(double global_scale, struct vela_x11_point x11);
struct vela_x11_rect vela_wayland_rect_to_x11(double global_scale, struct vela_x11_rect logical);
struct vela_x11_rect vela_x11_rect_to_wayland(double global_scale, struct vela_x11_rect x11);

#endif
