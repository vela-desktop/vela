// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "text.h"
#include "config.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
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

struct vela_text *vela_text_create(void)
{
    char family[256];
    double points = 10.0;
    kde_font(family, sizeof(family), &points);

    char file[PATH_MAX];
    int index = 0;
    if (!find_font(family, file, sizeof(file), &index)) {
        wlr_log(WLR_ERROR, "Text: no usable font, titles will have no text");
        return NULL;
    }
    struct vela_text *text = calloc(1, sizeof(*text));
    if (FT_Init_FreeType(&text->library) != 0 || FT_New_Face(text->library, file, index, &text->face) != 0) {
        wlr_log(WLR_ERROR, "Text: no usable font, titles will have no text");
        vela_text_destroy(text);
        return NULL;
    }
    text->font = hb_ft_font_create_referenced(text->face);
    text->pixel_size = points * 96.0 / 72.0;
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

// One channel "over" in premultiplied form: neighboring glyphs can touch.
static uint32_t over(uint32_t dst, int shift, double value, double coverage)
{
    double old = ((dst >> shift) & 0xff) / 255.0;
    return (uint32_t)lround(fmin(1.0, value * coverage + old * (1.0 - coverage)) * 255.0) << shift;
}

// The first `count` glyphs of `line`, from the pen onwards.
static void draw_glyphs(FT_Face face, const struct shaped *line, unsigned count, int64_t *pen, int ascender,
    const double color[4], struct vela_image *image)
{
    for (unsigned i = 0; i < count; ++i) {
        const hb_glyph_position_t *position = &line->positions[i];
        if (FT_Load_Glyph(face, line->infos[i].codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
            FT_GlyphSlot slot = face->glyph;
            const FT_Bitmap *bitmap = &slot->bitmap;
            int origin_x = (int)((*pen + position->x_offset + 32) >> 6) + slot->bitmap_left;
            int origin_y = ascender - (int)((position->y_offset + 32) >> 6) - slot->bitmap_top;
            for (unsigned row = 0; row < bitmap->rows; ++row) {
                int y = origin_y + (int)row;
                if (y < 0 || y >= image->height) {
                    continue;
                }
                for (unsigned col = 0; col < bitmap->width; ++col) {
                    int x = origin_x + (int)col;
                    if (x < 0 || x >= image->width) {
                        continue;
                    }
                    double coverage = bitmap->buffer[row * (unsigned)bitmap->pitch + col] / 255.0 * color[0];
                    if (coverage <= 0.0) {
                        continue;
                    }
                    uint32_t *dst = &image->pixels[(size_t)y * (size_t)image->width + (size_t)x];
                    *dst = over(*dst, 24, 1.0, coverage) | over(*dst, 16, color[1], coverage)
                        | over(*dst, 8, color[2], coverage) | over(*dst, 0, color[3], coverage);
                }
            }
        }
        *pen += position->x_advance;
    }
}

void vela_text_render(struct vela_text *text, const char *utf8, double pixel_size, uint32_t color, int max_width,
    struct vela_image *out)
{
    FT_Face face = text->face;
    FT_Set_Char_Size(face, 0, (FT_F26Dot6)lround(pixel_size * 64.0), 72, 72);
    hb_ft_font_changed(text->font);

    struct shaped line = shape(text->font, utf8);
    struct shaped ellipsis = { 0 };
    unsigned keep = line.count;
    int64_t width = line.width;
    // Too long: cut it and add "…".
    if (max_width > 0 && line.width > (int64_t)max_width * 64) {
        ellipsis = shape(text->font, "…");
        width = 0;
        keep = 0;
        while (keep < line.count && width + line.positions[keep].x_advance + ellipsis.width <= (int64_t)max_width * 64) {
            width += line.positions[keep].x_advance;
            ++keep;
        }
        width += ellipsis.width;
    }

    const FT_Size_Metrics *metrics = &face->size->metrics;
    int ascender = (int)ceil(metrics->ascender / 64.0);
    int descender = (int)floor(metrics->descender / 64.0);
    out->width = (int)ceil(width / 64.0) + 1;
    out->height = ascender - descender;
    if (out->width < 1) {
        out->width = 1;
    }
    if (out->height < 1) {
        out->height = 1;
    }
    out->pixels = calloc((size_t)out->width * (size_t)out->height, sizeof(uint32_t));

    // Alpha, red, green, blue, from 0 to 1.
    const double channels[4] = { ((color >> 24) & 0xff) / 255.0, ((color >> 16) & 0xff) / 255.0,
        ((color >> 8) & 0xff) / 255.0, (color & 0xff) / 255.0 };
    int64_t pen = 0;
    draw_glyphs(face, &line, keep, &pen, ascender, channels, out);
    if (ellipsis.buffer) {
        draw_glyphs(face, &ellipsis, ellipsis.count, &pen, ascender, channels, out);
        hb_buffer_destroy(ellipsis.buffer);
    }
    hb_buffer_destroy(line.buffer);
}
