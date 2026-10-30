// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include "x11_coordinates.h"

#include <math.h>

bool vela_x11_space_valid(struct vela_x11_space space)
{
    return isfinite(space.logical_x) && isfinite(space.logical_y)
        && isfinite(space.x11_x) && isfinite(space.x11_y)
        && isfinite(space.scale) && space.scale > 0.0;
}

struct vela_x11_point vela_x11_to_wayland(struct vela_x11_space space, struct vela_x11_point physical)
{
    return (struct vela_x11_point) {
        space.logical_x + (physical.x - space.x11_x) / space.scale,
        space.logical_y + (physical.y - space.x11_y) / space.scale,
    };
}

struct vela_x11_point vela_wayland_to_x11(struct vela_x11_space space, struct vela_x11_point logical)
{
    return (struct vela_x11_point) {
        space.x11_x + (logical.x - space.logical_x) * space.scale,
        space.x11_y + (logical.y - space.logical_y) * space.scale,
    };
}

struct vela_x11_point vela_x11_extent_to_wayland(struct vela_x11_space space, struct vela_x11_point extent)
{
    return (struct vela_x11_point) { extent.x / space.scale, extent.y / space.scale };
}

struct vela_x11_point vela_wayland_extent_to_x11(struct vela_x11_space space, struct vela_x11_point extent)
{
    return (struct vela_x11_point) { extent.x * space.scale, extent.y * space.scale };
}
