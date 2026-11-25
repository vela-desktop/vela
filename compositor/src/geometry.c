// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "geometry.h"

#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

float vela_scale_for_dpi(double phys_width_mm, double phys_height_mm, int width, int height, bool internal,
    double *dpi)
{
    *dpi = 0.0;
    if (phys_width_mm <= 0.0 || phys_height_mm <= 0.0 || width <= 0 || height <= 0) {
        return 1.0f;
    }
    double diagonal_mm = hypot(phys_width_mm, phys_height_mm);
    double aspect_error = fabs((phys_width_mm / phys_height_mm) / ((double)width / height) - 1.0);
    if (diagonal_mm < 8 * 25.4 || diagonal_mm > 100 * 25.4 || aspect_error > 0.1) {
        return 1.0f;
    }
    *dpi = hypot(width, height) / (diagonal_mm / 25.4);
    double reference = internal ? 105.6 : 96.0;
    return (float)vela_clampd(round(*dpi / reference * 4.0) / 4.0, 1.0, 3.0);
}

static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

bool vela_is_internal_panel(const char *name)
{
    return starts_with(name, "eDP") || starts_with(name, "LVDS") || starts_with(name, "DSI");
}

float vela_requested_scale(const char *value, const char *name)
{
    if (!value || !*value) {
        return 0.0f;
    }
    if (!strchr(value, '=')) {
        return strtof(value, NULL);
    }
    size_t name_length = strlen(name);
    for (const char *item = value; *item;) {
        const char *end = strchr(item, ',');
        if (!end) {
            end = item + strlen(item);
        }
        const char *eq = memchr(item, '=', (size_t)(end - item));
        if (eq && (size_t)(eq - item) == name_length && memcmp(item, name, name_length) == 0) {
            return strtof(eq + 1, NULL);
        }
        item = *end ? end + 1 : end;
    }
    return 0.0f;
}

int64_t vela_buffer_pixels(int64_t logical, double scale)
{
    int64_t scale120 = lround(scale * 120.0);
    return (logical * scale120 + 60) / 120;
}

struct vela_axis vela_place_axis(int start, int size, int screen, double scale, bool open_before, bool open_after)
{
    bool touches_start = start <= 0 && open_before;
    bool touches_end = start + size >= screen && open_after;
    int exact = 0;
    int above = 0; // the smallest that spills over
    int below = 1; // the largest that stays inside
    int64_t first = (int64_t)floor(size / scale) - 1;
    int64_t last = (int64_t)ceil(size / scale) + 1;
    for (int64_t w = first; w <= last; ++w) {
        if (w <= 0) {
            continue;
        }
        int64_t pixels = vela_buffer_pixels(w, scale);
        if (pixels == size) {
            exact = (int)w;
        } else if (pixels > size && !above) {
            above = (int)w;
        } else if (pixels < size) {
            below = (int)w;
        }
    }
    if (exact) {
        return (struct vela_axis) { start / scale, exact };
    }
    if ((touches_start || touches_end) && above) {
        // Spill over the output's side: past the end, or before the start if
        // the free edge is there.
        int64_t excess = vela_buffer_pixels(above, scale) - size;
        double from = touches_end ? start : (double)(start - excess);
        return (struct vela_axis) { from / scale, above };
    }
    return (struct vela_axis) { start / scale, below };
}

int vela_physical_edge(int logical, int full_start, int full_end, int pixels, double scale)
{
    if (logical <= full_start) {
        return 0;
    }
    if (logical >= full_end) {
        return pixels;
    }
    return (int)lround((logical - full_start) * scale);
}

struct vela_pixel_box vela_edges_to_pixels(double x, double y, double width, double height, double origin_x,
    double origin_y, double scale)
{
    int x1 = (int)lround((x - origin_x) * scale);
    int y1 = (int)lround((y - origin_y) * scale);
    int x2 = (int)lround((x + width - origin_x) * scale);
    int y2 = (int)lround((y + height - origin_y) * scale);
    return (struct vela_pixel_box) { x1, y1, x2 - x1, y2 - y1 };
}

bool vela_buffer_matches_area(double src_x, double src_y, double buffer_width, double buffer_height, double width,
    double height, double scale)
{
    bool whole = src_x == floor(src_x) && src_y == floor(src_y) && buffer_width == floor(buffer_width)
        && buffer_height == floor(buffer_height);
    return whole && fabs(buffer_width - width * scale) < 1.0 && fabs(buffer_height - height * scale) < 1.0;
}

int vela_whole_pixel_units(float scale)
{
    for (int units = 1; units <= 64; ++units) {
        double pixels = units * (double)scale; // exact: a float times a small integer
        if (pixels > 0.0 && pixels == floor(pixels)) {
            return units;
        }
    }
    return 0;
}

bool vela_one_to_one(double src_x, double src_y, double src_width, double src_height, int box_width,
    int box_height, bool swapped)
{
    double width = swapped ? src_height : src_width;
    double height = swapped ? src_width : src_height;
    return src_x == floor(src_x) && src_y == floor(src_y) && width == (double)box_width
        && height == (double)box_height;
}
