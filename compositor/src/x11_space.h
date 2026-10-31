// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_X11_SPACE_H
#define VELA_X11_SPACE_H

// The X11 root as Xwayland sees it (docs/renderer.md §3.9): every output is
// as large as its pixels, so an X11 app (a game) finds the native resolution
// in RandR and draws 1:1. An X11 pixel of a window is a physical pixel of its
// output, 1/scale logical units. Pure functions, tested in
// compositor/tests/x11_space_test.cpp.

#include <stdbool.h>
#include <stddef.h>

struct vela_x11_output {
    // In: the output in the logical layout, its scale and its pixels (after
    // rotation).
    int x, y, width, height;
    double scale;
    int pixel_width, pixel_height;
    // Out (vela_x11_place): its rectangle in the X11 root.
    int root_x, root_y;
};

// Places the outputs in the root, each as large as its pixels. The origins are
// the logical ones, from the layout's top-left corner, multiplied by the
// largest pixel density: outputs side by side in the layout never overlap in
// the root, and those with the largest scale stay adjacent.
void vela_x11_place(struct vela_x11_output *outputs, size_t count);

// The output holding a root point, or the nearest one. NULL only without
// outputs.
const struct vela_x11_output *vela_x11_output_at(const struct vela_x11_output *outputs, size_t count, double x,
    double y);

// From logical coordinates to the root and back, through one output.
void vela_x11_from_logical(const struct vela_x11_output *output, double lx, double ly, double *x, double *y);
void vela_x11_to_logical(const struct vela_x11_output *output, double x, double y, double *lx, double *ly);

// Xft.dpi for X11 apps, which then scale themselves: 96 at the largest scale.
int vela_x11_dpi(const struct vela_x11_output *outputs, size_t count);

// Whether X resources (the root's RESOURCE_MANAGER, `length` bytes, not
// terminated) set Xft.dpi.
bool vela_x11_resources_set_dpi(const char *resources, size_t length);

#endif
