// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_GEOMETRY_H
#define VELA_GEOMETRY_H

// The geometry of sharpness (docs/renderer.md §3): the default scale from DPI,
// VELA_SCALE, and how a window's logical size is chosen so its buffer covers
// exactly the pixels of its area. Pure functions, tested in
// compositor/tests/geometry_test.cpp.

#include <stdbool.h>
#include <stdint.h>

// The default scale, like Windows (§3.8): from the output's DPI, in 25% steps,
// between 100% and 300%. Laptop panels are viewed from closer: reference 105.6
// DPI instead of 96. Nonsense sizes (projectors, TVs, adapters that make up
// the EDID: diagonal outside 8"–100", or proportions unlike the pixels') give
// 100%, and *dpi 0.
float vela_scale_for_dpi(double phys_width_mm, double phys_height_mm, int width, int height, bool internal,
    double *dpi);

// A built-in panel (laptop, tablet), from the connector name.
bool vela_is_internal_panel(const char *name);

// The scale asked with VELA_SCALE for output `name`: "1.25" for all, or
// "DP-1=1.5,HDMI-A-1=1". 0 if none.
float vela_requested_scale(const char *value, const char *name);

// The buffer pixels a client draws for `logical` units at that scale
// (fractional-scale-v1, in 120ths, rounding half away).
int64_t vela_buffer_pixels(int64_t logical, double scale);

struct vela_axis {
    double offset; // logical position relative to the output origin
    int size; // logical size
};

// One axis of a window that must cover pixels [start, start + size) of an
// output `screen` pixels wide (§3.5): the logical size whose buffer is exactly
// `size` pixels. When there is none (at 150% 2560 pixels would be 1706.67
// units), the extra pixel spills off the output on a free side (no other
// output there: open_before and open_after); otherwise it stays one pixel
// inside.
struct vela_axis vela_place_axis(int start, int size, int screen, double scale, bool open_before, bool open_after);

// An edge of the area left free by panels, in output pixels: those touching
// the output edges stay exactly on its pixels.
int vela_physical_edge(int logical, int full_start, int full_end, int pixels, double scale);

// A rectangle in output pixels.
struct vela_pixel_box {
    int x, y, width, height;
};

// A logical rectangle in pixels (§3.2): the edges are rounded, not position
// and size, so adjacent rectangles stay adjacent. Halves round up everywhere,
// so moving a rectangle by whole pixels moves its pixels by as many, also
// across the output's left or top edge.
struct vela_pixel_box vela_edges_to_pixels(double x, double y, double width, double height, double origin_x,
    double origin_y, double scale);

// The app drew at the output scale (§3.4): the buffer, at integer coordinates,
// is as large as its physical area up to rounding. Then the surface covers
// exactly the buffer's pixels.
bool vela_buffer_matches_area(double src_x, double src_y, double buffer_width, double buffer_height, double width,
    double height, double scale);

// 1:1 copy, no resampling (§3.3): each buffer pixel lands exactly on an output
// pixel. `swapped`: rotated by 90°.
bool vela_one_to_one(double src_x, double src_y, double src_width, double src_height, int box_width,
    int box_height, bool swapped);

// The fewest logical units, up to 64, that are a whole number of physical
// pixels at `scale` (1 at 100% and 200%, 2 at 150%, 4 at 125%, 175% and 225%),
// 0 if there are none. An image moved by a whole number of them keeps the rest
// of its content on the same pixels at every position, exactly (also across
// the output's edge, vela_edges_to_pixels): moved by the equivalent fraction
// (pixels / scale), it would round some positions the other way (a title's
// ink margin, decoration.c).
int vela_whole_pixel_units(float scale);

#endif
