// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include "x11_coordinates.h"

#include <assert.h>
#include <math.h>

static void same(double actual, double expected)
{
    assert(fabs(actual - expected) < 1e-8);
}

static void check_inverse(struct vela_x11_space output, struct vela_x11_point logical)
{
    struct vela_x11_point physical = vela_wayland_to_x11(output, logical);
    struct vela_x11_point result = vela_x11_to_wayland(output, physical);
    same(result.x, logical.x);
    same(result.y, logical.y);
}

int main(void)
{
    // Real DP-3 + HDMI-A-1 layout. HDMI (100%) above DP (125%).
    // X11 physical resolution on DP: 2560x1440; Wayland 2048x1152.
    const struct vela_x11_space hdmi = {
        .logical_x=0, .logical_y=0, .x11_x=0, .x11_y=0, .scale=1
    };
    const struct vela_x11_space dp = {
        .logical_x=0, .logical_y=1080, .x11_x=0, .x11_y=1080, .scale=1.25
    };
    assert(vela_x11_space_valid(dp));
    struct vela_x11_point bottom = vela_wayland_to_x11(
        dp, (struct vela_x11_point) {2048, 2232});
    same(bottom.x, 2560);
    same(bottom.y, 2520);
    struct vela_x11_point display = vela_x11_to_wayland(
        dp, (struct vela_x11_point) {2560, 2520});
    same(display.x, 2048);
    same(display.y, 2232);
    same(vela_wayland_extent_to_x11(dp,
        (struct vela_x11_point) {2048,1152}).x, 2560);
    same(vela_wayland_extent_to_x11(dp,
        (struct vela_x11_point) {2048,1152}).y, 1440);
    for (int x=0; x<=2048; x+=32)
        for (int y=1080; y<=2232; y+=72)
            check_inverse(dp, (struct vela_x11_point) {x,y});
    check_inverse(hdmi, (struct vela_x11_point) {1920,1080});

    // Negative-origin layouts cannot use one global DPI multiplier.
    const struct vela_x11_space left = {
        .logical_x=-2048, .logical_y=-1152,
        .x11_x=-2560, .x11_y=-1440, .scale=1.25
    };
    const struct vela_x11_point left_origin = vela_wayland_to_x11(
        left, (struct vela_x11_point) {-2048,-1152});
    same(left_origin.x,-2560);
    same(left_origin.y,-1440);
    check_inverse(left, (struct vela_x11_point) {-800,-900});
    assert(!vela_x11_space_valid((struct vela_x11_space) {.scale=0}));
    assert(!vela_x11_space_valid((struct vela_x11_space) {.scale=NAN}));
    return 0;
}
