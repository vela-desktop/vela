// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VELA_X11_COORDINATES_H
#define VELA_X11_COORDINATES_H

#include <stdbool.h>

// Per-output X11 pixel coordinates and Wayland logical coordinates are
// distinct spaces. The X11 origin is computed by an output-layout adapter;
// never infer it from the Wayland logical origin on mixed-scale screens.
struct vela_x11_space {
    double logical_x, logical_y;
    double x11_x, x11_y;
    double scale; // physical X11 pixels per Wayland logical unit
};

struct vela_x11_point {
    double x, y;
};

bool vela_x11_space_valid(struct vela_x11_space space);
// Map a point relative to a particular output, not an arbitrary window.
// Callers select the output in the INPUT coordinate space first.
struct vela_x11_point vela_x11_to_wayland(struct vela_x11_space space, struct vela_x11_point physical);
struct vela_x11_point vela_wayland_to_x11(struct vela_x11_space space, struct vela_x11_point logical);
// Convert widths / heights; never use these for global screen positions.
struct vela_x11_point vela_x11_extent_to_wayland(struct vela_x11_space space, struct vela_x11_point extent);
struct vela_x11_point vela_wayland_extent_to_x11(struct vela_x11_space space, struct vela_x11_point extent);
#endif
