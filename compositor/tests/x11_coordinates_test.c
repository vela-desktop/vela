// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include "x11_coordinates.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void near(double actual, double expected)
{
    if (!isfinite(actual) || fabs(actual - expected) > 1e-8) {
        fprintf(stderr, "coordinate mismatch: actual %.8f, expected %.8f\n", actual, expected);
        exit(EXIT_FAILURE);
    }
}

static void check_inverse(double scale, struct vela_x11_rect rect)
{
    const struct vela_x11_rect x11 = vela_wayland_rect_to_x11(scale, rect);
    const struct vela_x11_rect back = vela_x11_rect_to_wayland(scale, x11);
    near(back.x, rect.x);
    near(back.y, rect.y);
    near(back.width, rect.width);
    near(back.height, rect.height);
}

int main(void)
{
    const double outputs[] = {1.25, 1.0};
    const double scaled = vela_x11_global_scale(VELA_X11_APPLY_SCALING_THEMSELVES,
        outputs, sizeof(outputs) / sizeof(outputs[0]));
    near(scaled, 1.25);
    near(vela_x11_global_scale(VELA_X11_SCALED_BY_SYSTEM, outputs, 2), 1.0);
    near(vela_x11_global_scale(VELA_X11_APPLY_SCALING_THEMSELVES, NULL, 0), 1.0);
    const double malformed[] = {NAN, -1.0, INFINITY, 1.5};
    near(vela_x11_global_scale(VELA_X11_APPLY_SCALING_THEMSELVES, malformed, 4), 1.5);

    // Physical DP-3 2560x1440@125%, logical (0,1080)-(2048,2232).
    // HDMI-A-1 above it: 1920x1080@100%, logical (0,0)-(1920,1080).
    // KWin uses a SINGLE global scale 1.25: on HDMI X11 uses an
    // *oversampled* virtual 2400x1350 rectangle. This is intentional and
    // keeps the X11 root geometry contiguous across screens.
    const struct vela_x11_rect dp = {0,1080,2048,1152};
    const struct vela_x11_rect hdmi = {0,0,1920,1080};
    struct vela_x11_rect xdp = vela_wayland_rect_to_x11(scaled, dp);
    struct vela_x11_rect xhdmi = vela_wayland_rect_to_x11(scaled, hdmi);
    near(xdp.x,0);
    near(xdp.y,1350);
    near(xdp.width,2560);
    near(xdp.height,1440);
    near(xhdmi.width,2400);
    near(xhdmi.height,1350);
    near(xhdmi.y+xhdmi.height,xdp.y); // no gap or overlap at the seam
    check_inverse(scaled, dp);
    check_inverse(scaled, hdmi);

    // Under "scaled by system" nothing changes for the existing compositor
    // compatibility path, on which Steam buttons and fullscreen already work.
    const struct vela_x11_rect compat = vela_wayland_rect_to_x11(1.0, dp);
    near(compat.width,2048);
    near(compat.height,1152);
    for (int x=-2560;x<=2560;x+=113) {
        struct vela_x11_point in = {(double)x,-720.25};
        struct vela_x11_point back = vela_x11_to_wayland(scaled,
            vela_wayland_to_x11(scaled,in));
        near(back.x,in.x);
        near(back.y,in.y);
    }
    return EXIT_SUCCESS;
}
