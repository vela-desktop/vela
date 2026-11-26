// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "text.h"
#include "alloc/alloc.h"
#include "config.h"
#include "util.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_BITMAP_H
#include <hb-ft.h>
#include <hb.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wlr/util/log.h>

struct vela_text {
    FT_Library library;
    FT_Face face;
    hb_font_t *font;
    double pixel_size;
};

// The font chosen in KDE: [WM] activeFont, otherwise [General] font, in Qt's
// "family,points,..." format; otherwise KDE's default.
static void kde_font(char *family, size_t size, double *points)
{
    snprintf(family, size, "Noto Sans");
    *points = 10.0;
    const char *config = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    if (config && *config) {
        snprintf(path, sizeof(path), "%s/kdeglobals", config);
    } else if (home) {
        snprintf(path, sizeof(path), "%s/.config/kdeglobals", home);
    } else {
        return;
    }
    static const char *const wm_key[] = { "activeFont" };
    static const char *const general_key[] = { "font" };
    char wm[1][256];
    char general[1][256];
    // Missing, unreadable or read only in part: KDE's default (a partial
    // read could pick the wrong font).
    if (!vela_ini_read(path, "WM", wm_key, wm, 1) || !vela_ini_read(path, "General", general_key, general, 1)) {
        return;
    }
    const char *wm_font = wm[0];
    const char *general_font = general[0];
    const char *chosen = wm_font[0] ? wm_font : general_font;
    if (!chosen[0]) {
        return;
    }
    const char *comma = strchr(chosen, ',');
    snprintf(family, size, "%.*s", comma ? (int)(comma - chosen) : (int)strlen(chosen), chosen);
    if (comma) {
        *points = fmax(6.0, atof(comma + 1));
    }
}

// The font file for a family, as the rest of the system would choose it
// (fontconfig).
static bool find_font(const char *family, char *file, size_t size, int *index)
{
    if (!FcInit()) {
        return false;
    }
    FcPattern *pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)family);
    FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_REGULAR);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match) {
        return false;
    }
    FcChar8 *path = NULL;
    bool ok = FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch;
    if (ok) {
        snprintf(file, size, "%s", (const char *)path);
        *index = 0;
        FcPatternGetInteger(match, FC_INDEX, 0, index);
    }
    FcPatternDestroy(match);
    return ok;
}

struct vela_text *vela_text_create_file(const char *file, int index, double points)
{
    struct vela_text *text = VELA_NEW(struct vela_text);
    if (FT_Init_FreeType(&text->library) != 0 || FT_New_Face(text->library, file, index, &text->face) != 0) {
        vela_text_destroy(text);
        return NULL;
    }
    // HarfBuzz lays the text out with unhinted advances (hb-ft's default
    // load flags); draw_glyphs starts each glyph on the nearest pixel.
    text->font = hb_ft_font_create_referenced(text->face);
    text->pixel_size = points * 96.0 / 72.0;
    return text;
}

struct vela_text *vela_text_create(void)
{
    char family[256];
    double points = 10.0;
    kde_font(family, sizeof(family), &points);

    char file[PATH_MAX];
    int index = 0;
    struct vela_text *text =
        find_font(family, file, sizeof(file), &index) ? vela_text_create_file(file, index, points) : NULL;
    if (!text) {
        wlr_log(WLR_ERROR, "Text: no usable font, titles will have no text");
        return NULL;
    }
    wlr_log(WLR_INFO, "Text: %s %.1f pt (%s)", family, points, file);
    return text;
}

void vela_text_destroy(struct vela_text *text)
{
    if (!text) {
        return;
    }
    if (text->font) {
        hb_font_destroy(text->font);
    }
    if (text->face) {
        FT_Done_Face(text->face);
    }
    if (text->library) {
        FT_Done_FreeType(text->library);
    }
    free(text);
}

double vela_text_pixel_size(const struct vela_text *text)
{
    return text->pixel_size;
}

static void set_size(struct vela_text *text, double pixel_size)
{
    FT_Set_Char_Size(text->face, 0, (FT_F26Dot6)lround(pixel_size * 64.0), 72, 72);
    hb_ft_font_changed(text->font);
}

// The line box's ascender and descender (negative), in whole pixels.
static void line_metrics(const struct vela_text *text, int *ascender, int *descender)
{
    const FT_Size_Metrics *metrics = &text->face->size->metrics;
    *ascender = (int)ceil(metrics->ascender / 64.0);
    *descender = (int)floor(metrics->descender / 64.0);
}

// Division rounding down and up (C's rounds toward zero; 26.6 positions can
// be negative).
static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return q - (a % b != 0 && (a < 0) != (b < 0));
}

static int64_t ceil_div(int64_t a, int64_t b)
{
    return -floor_div(-a, b);
}

static int clamp_int(int64_t value)
{
    return value < INT_MIN ? INT_MIN : value > INT_MAX ? INT_MAX : (int)value;
}

// A line laid out by HarfBuzz; the width in 26.6.
struct shaped {
    hb_buffer_t *buffer;
    hb_glyph_info_t *infos;
    hb_glyph_position_t *positions;
    unsigned count;
    int64_t width;
};

static struct shaped shape(hb_font_t *font, const char *utf8)
{
    struct shaped out = { .buffer = hb_buffer_create() };
    int length = (int)strlen(utf8);
    hb_buffer_add_utf8(out.buffer, utf8, length, 0, length);
    hb_buffer_guess_segment_properties(out.buffer);
    hb_shape(font, out.buffer, NULL, 0);
    out.infos = hb_buffer_get_glyph_infos(out.buffer, &out.count);
    out.positions = hb_buffer_get_glyph_positions(out.buffer, &out.count);
    for (unsigned i = 0; i < out.count; ++i) {
        out.width += out.positions[i].x_advance;
    }
    return out;
}

// Too long for `max_width` (0: no limit): cut, with "…" (shaped into
// `ellipsis`) after the first `keep` glyphs, `width` wide with it (26.6).
// Returns whether it was cut; if not, `keep` and `width` are left as they are.
static bool cut_line(
    hb_font_t *font, const struct shaped *line, int max_width, struct shaped *ellipsis, unsigned *keep, int64_t *width)
{
    if (max_width <= 0 || line->width <= (int64_t)max_width * 64) {
        return false;
    }
    *ellipsis = shape(font, "…");
    *width = 0;
    *keep = 0;
    while (
        *keep < line->count && *width + line->positions[*keep].x_advance + ellipsis->width <= (int64_t)max_width * 64) {
        *width += line->positions[*keep].x_advance;
        ++*keep;
    }
    *width += ellipsis->width;
    return true;
}

// The max_width values that keep the same glyphs of `line` (or all of them),
// so the same image: the inverse of the cut in vela_text_render.
static void fit(
    const struct shaped *line, const struct shaped *ellipsis, unsigned keep, bool cut, struct vela_text_layout *layout)
{
    if (!cut) {
        layout->fit_min = clamp_int(ceil_div(line->width, 64) > 1 ? ceil_div(line->width, 64) : 1);
        layout->fit_max = INT_MAX;
        return;
    }
    // Each glyph kept, with the ellipsis, must fit; the next one must not,
    // and the whole line must still be too wide.
    int64_t low = 1;
    int64_t width = 0;
    for (unsigned i = 0; i < keep; ++i) {
        width += line->positions[i].x_advance;
        int64_t needed = ceil_div(width + ellipsis->width, 64);
        low = needed > low ? needed : low;
    }
    int64_t high = floor_div(line->width - 1, 64);
    if (keep < line->count) {
        int64_t next = floor_div(width + line->positions[keep].x_advance + ellipsis->width - 1, 64);
        high = next < high ? next : high;
    }
    layout->fit_min = clamp_int(low);
    layout->fit_max = clamp_int(high);
}

bool vela_text_fits(const struct vela_text_layout *layout, int max_width)
{
    if (max_width <= 0) {
        return max_width == 0 && layout->fit_max == INT_MAX;
    }
    return max_width >= layout->fit_min && max_width <= layout->fit_max;
}

// A glyph drawn for the line: 8-bit coverage, its top-left corner from the
// line box's.
struct placed {
    uint8_t *coverage;
    int width;
    int rows;
    int x;
    int y;
};

// The slot's bitmap as coverage from 0 to 255. False if it has no pixels (a
// space) or none we can draw (a color glyph). Bitmap fonts and embedded
// strikes give 1-bit (or 2- and 4-bit) bitmaps: converted, never read as
// bytes.
static bool take_coverage(FT_Library library, const FT_Bitmap *bitmap, struct placed *out)
{
    if (bitmap->width == 0 || bitmap->rows == 0 || bitmap->pixel_mode == FT_PIXEL_MODE_BGRA) {
        return false;
    }
    FT_Bitmap gray;
    FT_Bitmap_Init(&gray);
    const FT_Bitmap *source = bitmap;
    if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY || bitmap->pitch < 0) {
        if (FT_Bitmap_Convert(library, bitmap, &gray, 1) != 0) {
            FT_Bitmap_Done(library, &gray);
            return false;
        }
        source = &gray;
    }
    // The levels the bitmap uses: 0..255 rendered, 0..1 for a 1-bit one.
    unsigned top = source->num_grays > 1 ? (unsigned)source->num_grays - 1 : 1;
    out->width = (int)source->width;
    out->rows = (int)source->rows;
    out->coverage = VELA_NEW_N(uint8_t, vela_size_mul(source->width, source->rows));
    for (unsigned row = 0; row < source->rows; ++row) {
        const unsigned char *line = source->buffer + (size_t)row * (size_t)source->pitch;
        for (unsigned col = 0; col < source->width; ++col) {
            unsigned value = line[col] < top ? line[col] : top;
            out->coverage[(size_t)row * source->width + col] = (uint8_t)((value * 255 + top / 2) / top);
        }
    }
    FT_Bitmap_Done(library, &gray);
    return true;
}

// The first `count` glyphs of `line`, from the pen onwards (26.6, from the
// line box's left edge), appended to `glyphs`.
static void draw_glyphs(struct vela_text *text, const struct shaped *line, unsigned count, int64_t *pen, int ascender,
    struct placed *glyphs, unsigned *drawn)
{
    FT_Face face = text->face;
    for (unsigned i = 0; i < count; ++i) {
        const hb_glyph_position_t *position = &line->positions[i];
        // Each glyph on the nearest whole pixel, as FreeType rasterizes it.
        // Moving the outline by the rest of HarfBuzz's position would spread
        // a stem over two columns as often as it would sharpen one: Light
        // hinting leaves the horizontal axis alone, so where a stem falls
        // within its pixels is the font's design, kept at the whole-pixel pen
        // (docs/font-rendering-improvements.md, "Subpixel positioning").
        int64_t whole = floor_div(*pen + position->x_offset + 32, 64);
        // Vertically on whole pixels, as hinted.
        int y = ascender - (int)floor_div(position->y_offset + 32, 64);
        *pen += position->x_advance;
        if (FT_Load_Glyph(face, line->infos[i].codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) {
            continue;
        }
        FT_GlyphSlot slot = face->glyph;
        struct placed *glyph = &glyphs[*drawn];
        if (take_coverage(text->library, &slot->bitmap, glyph)) {
            glyph->x = clamp_int(whole + slot->bitmap_left);
            glyph->y = y - slot->bitmap_top;
            ++*drawn;
        }
    }
}

// One channel "over" in premultiplied form: neighboring glyphs can touch.
static uint32_t over(uint32_t dst, int shift, double value, double coverage)
{
    double old = ((dst >> shift) & 0xff) / 255.0;
    return (uint32_t)lround(fmin(1.0, value * coverage + old * (1.0 - coverage)) * 255.0) << shift;
}

static void composite(const struct placed *glyph, int left, int top, const double color[4], struct vela_image *image)
{
    for (int row = 0; row < glyph->rows; ++row) {
        int y = top + glyph->y + row;
        if (y < 0 || y >= image->height) {
            continue;
        }
        for (int col = 0; col < glyph->width; ++col) {
            int x = left + glyph->x + col;
            if (x < 0 || x >= image->width) {
                continue;
            }
            double coverage = glyph->coverage[(size_t)row * (size_t)glyph->width + (size_t)col] / 255.0 * color[0];
            if (coverage <= 0.0) {
                continue;
            }
            uint32_t *dst = &image->pixels[(size_t)y * (size_t)image->width + (size_t)x];
            *dst = over(*dst, 24, 1.0, coverage) | over(*dst, 16, color[1], coverage)
                | over(*dst, 8, color[2], coverage) | over(*dst, 0, color[3], coverage);
        }
    }
}

void vela_text_render(struct vela_text *text, const char *utf8, double pixel_size, uint32_t color, int max_width,
    int margin, struct vela_image *out, struct vela_text_layout *layout)
{
    set_size(text, pixel_size);

    struct shaped line = shape(text->font, utf8);
    struct shaped ellipsis = { 0 };
    unsigned keep = line.count;
    int64_t width = line.width;
    bool cut = cut_line(text->font, &line, max_width, &ellipsis, &keep, &width);
    fit(&line, &ellipsis, keep, cut, layout);

    int ascender = 0;
    int descender = 0;
    line_metrics(text, &ascender, &descender);
    layout->ascender = ascender;
    layout->line_width = vela_max(1, clamp_int(ceil_div(width, 64)));
    layout->line_height = vela_max(1, ascender - descender);

    unsigned count = keep + ellipsis.count;
    struct placed *glyphs = VELA_NEW_N(struct placed, count);
    unsigned drawn = 0;
    int64_t pen = 0;
    draw_glyphs(text, &line, keep, &pen, ascender, glyphs, &drawn);
    if (ellipsis.buffer) {
        draw_glyphs(text, &ellipsis, ellipsis.count, &pen, ascender, glyphs, &drawn);
    }

    // The ink's bounds and the old image's: the line box and a column. Up to
    // an em outside the box: real fonts stay well within it, and a title
    // stacking marks (which an app can send) can't ask for a huge image.
    int reach = (int)ceil(pixel_size) + 1;
    int left = 0;
    int top = 0;
    int right = vela_max(1, clamp_int(ceil_div(width, 64) + 1)); // the old image's width
    int bottom = layout->line_height;
    for (unsigned i = 0; i < drawn; ++i) {
        left = vela_min(left, glyphs[i].x);
        top = vela_min(top, glyphs[i].y);
        right = vela_max(right, clamp_int((int64_t)glyphs[i].x + glyphs[i].width));
        bottom = vela_max(bottom, clamp_int((int64_t)glyphs[i].y + glyphs[i].rows));
    }
    left = vela_max(left, -reach);
    top = vela_max(top, -reach);
    right = vela_min(right, layout->line_width + reach);
    bottom = vela_min(bottom, layout->line_height + reach);
    // Left and above in whole margins, so the caller can keep the line box in
    // place (text.h).
    int step = margin > 0 ? margin : 1;
    layout->line_x = (int)ceil_div(-left, step) * step;
    layout->line_y = (int)ceil_div(-top, step) * step;
    out->width = layout->line_x + right;
    out->height = layout->line_y + bottom;
    out->pixels = VELA_NEW_N(uint32_t, vela_size_mul((size_t)out->width, (size_t)out->height));

    // Alpha, red, green, blue, from 0 to 1.
    const double channels[4] = { ((color >> 24) & 0xff) / 255.0, ((color >> 16) & 0xff) / 255.0,
        ((color >> 8) & 0xff) / 255.0, (color & 0xff) / 255.0 };
    for (unsigned i = 0; i < drawn; ++i) {
        composite(&glyphs[i], layout->line_x, layout->line_y, channels, out);
        free(glyphs[i].coverage);
    }
    free(glyphs);
    if (ellipsis.buffer) {
        hb_buffer_destroy(ellipsis.buffer);
    }
    hb_buffer_destroy(line.buffer);
}
