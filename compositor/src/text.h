// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_TEXT_H
#define VELA_TEXT_H

// Text drawn by the compositor (window titles). FreeType for the glyphs,
// HarfBuzz to lay them out, fontconfig to find the font chosen in KDE
// (docs/renderer.md §9.5). Text is always rasterized at the exact physical
// size: never scaled, sharp at every scale. Each glyph starts on a whole
// pixel, as FreeType rasterizes it, and no glyph's ink is cut off
// (docs/font-rendering-improvements.md).

#include "image.h"

#include <stdbool.h>
#include <stdint.h>

struct vela_text;

// Where the text lies in an image made by vela_text_render, in its pixels.
struct vela_text_layout {
    // The line box: as wide as the text's advance, from the font's ascender
    // to its descender, the pen starting at its left edge and the baseline
    // `ascender` rows below its top. Ink can leave it (a "j"'s tail, an
    // italic's overhang, accents above capitals): the image is larger on
    // that side, so the line box's top-left corner is (line_x, line_y).
    int line_x;
    int line_y;
    int line_width;
    int line_height;
    int ascender;
    // The `max_width` values that make this same image: from fit_min to
    // fit_max (INT_MAX when the text isn't cut: then 0 too).
    int fit_min;
    int fit_max;
};

// Loads KDE's font. NULL if no usable font exists.
struct vela_text *vela_text_create(void);
// A font file (face `index` in it) at `points`, whatever KDE chose: for the
// tests and for comparisons that need the same font everywhere. NULL if it
// can't be loaded.
struct vela_text *vela_text_create_file(const char *file, int index, double points);
void vela_text_destroy(struct vela_text *text);

// KDE's font size, in logical pixels (10 pt = 13.33 px).
double vela_text_pixel_size(const struct vela_text *text);

// One line of text in `color` (0xAARRGGBB), `pixel_size` physical pixels high,
// at most `max_width` wide (otherwise cut with "…"; 0: no limit). Ink outside
// the line box makes the image larger: to the left and above by a multiple of
// `margin` pixels (at least 1), so the caller can move the image by whole
// logical units and keep the line box on the same pixels (geometry.h,
// vela_whole_pixel_units). The image is freed with vela_image_finish.
void vela_text_render(struct vela_text *text, const char *utf8, double pixel_size, uint32_t color, int max_width,
    int margin, struct vela_image *out, struct vela_text_layout *layout);

// Whether vela_text_render, given `max_width` instead, would make the same
// image as the one `layout` describes (all else being equal).
bool vela_text_fits(const struct vela_text_layout *layout, int max_width);

#endif
