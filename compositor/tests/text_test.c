// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The compositor's text (text.c; docs/font-rendering-improvements.md),
// measured against the renderer it replaced: legacy_render below is text.c as
// it was before that change, line for line. Every improvement is measured on
// both, on the same font, sizes and strings: the old one must show the defect
// (so the measurement can see it), the new one must be better by a wide
// margin, and what didn't need to change must not have changed.
//
// - Positions: each glyph's ink is where HarfBuzz put the glyph. The old
//   renderer rounded every glyph to a whole pixel: up to half a pixel off,
//   so the gaps between letters varied.
// - Ink: nothing a glyph draws is cut off. The old image was the line box,
//   which a "j"'s tail, an italic's overhang or accents above capitals leave.
//   Inside the old box, a glyph at a whole-pixel position keeps its pixels.
// - Margins: whole steps of the caller's margin, as small as possible, and
//   the same ink relative to the line box whatever the margin; at most an em
//   past the line box, whatever an app puts in its title.
// - Resizing: a width in [fit_min, fit_max] makes the same image, and one
//   outside it a different one; drawn again only outside it, an interactive
//   resize draws the title a few times instead of on every frame.
// - Bitmap glyphs (a bitmap font written here): exactly their pixels. The old
//   renderer read 1-bit rows as bytes.
//
// The fonts are subsets of Noto Sans 2.015 (KDE's default font) in
// tests/fonts, hinting included, so every machine measures the same font.
// `--capture-dir DIR` also writes the old and new images as PGM files.

#include "text.h"

#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// CTest builds with NDEBUG in RelWithDebInfo, so assert() would skip checks.
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            fprintf(stderr, "FAILED: %s (%s:%d)\n", #expr, __FILE__, __LINE__);                                        \
            exit(EXIT_FAILURE);                                                                                        \
        }                                                                                                              \
    } while (0)

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define WHITE 0xffffffffu

// KDE's default size (10 pt = 13.33 logical pixels) at the scales Vela offers.
static const double scales[] = { 1.0, 1.25, 1.5, 1.75, 2.0, 2.25 };
#define KDE_PIXELS (10.0 * 96.0 / 72.0)

// "x" with a circumflex, a tilde and an acute stacked above it.
#define STACKED "x\u0302\u0303\u0301"
// Ink leaving the line box to the left ("j", the italic's slant), above (the
// stacked marks) and below.
#define MARGIN_TEXT "ja" STACKED "ʃ ƒ"

static char fonts[PATH_MAX];
static const char *capture_dir;
static FT_Library library;

static void font_path(char *path, size_t size, const char *name)
{
    CHECK(snprintf(path, size, "%s/%s", fonts, name) < (int)size);
}

// The same font, for the old renderer and for HarfBuzz's positions.
struct font {
    FT_Face face;
    hb_font_t *hb;
    struct vela_text *text;
};

static struct font open_font(const char *file)
{
    struct font font = { 0 };
    CHECK(FT_New_Face(library, file, 0, &font.face) == 0);
    font.hb = hb_ft_font_create_referenced(font.face);
    font.text = vela_text_create_file(file, 0, 10.0);
    CHECK(font.text);
    return font;
}

static void close_font(struct font *font)
{
    vela_text_destroy(font->text);
    hb_font_destroy(font->hb);
    FT_Done_Face(font->face);
}

static void set_size(struct font *font, double pixel_size)
{
    FT_Set_Char_Size(font->face, 0, (FT_F26Dot6)lround(pixel_size * 64.0), 72, 72);
    hb_ft_font_changed(font->hb);
}

// ---- The old renderer: text.c before the change, line for line ----

struct legacy_shaped {
    hb_buffer_t *buffer;
    hb_glyph_info_t *infos;
    hb_glyph_position_t *positions;
    unsigned count;
    int64_t width;
};

static struct legacy_shaped legacy_shape(hb_font_t *font, const char *utf8)
{
    struct legacy_shaped out = { .buffer = hb_buffer_create() };
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

static uint32_t legacy_over(uint32_t dst, int shift, double value, double coverage)
{
    double old = ((dst >> shift) & 0xff) / 255.0;
    return (uint32_t)lround(fmin(1.0, value * coverage + old * (1.0 - coverage)) * 255.0) << shift;
}

static void legacy_draw_glyphs(FT_Face face, const struct legacy_shaped *line, unsigned count, int64_t *pen,
    int ascender, const double color[4], struct vela_image *image)
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
                    *dst = legacy_over(*dst, 24, 1.0, coverage) | legacy_over(*dst, 16, color[1], coverage)
                        | legacy_over(*dst, 8, color[2], coverage) | legacy_over(*dst, 0, color[3], coverage);
                }
            }
        }
        *pen += position->x_advance;
    }
}

static void legacy_render(
    struct font *font, const char *utf8, double pixel_size, uint32_t color, int max_width, struct vela_image *out)
{
    FT_Face face = font->face;
    set_size(font, pixel_size);
    struct legacy_shaped line = legacy_shape(font->hb, utf8);
    struct legacy_shaped ellipsis = { 0 };
    unsigned keep = line.count;
    int64_t width = line.width;
    if (max_width > 0 && line.width > (int64_t)max_width * 64) {
        ellipsis = legacy_shape(font->hb, "…");
        width = 0;
        keep = 0;
        while (
            keep < line.count && width + line.positions[keep].x_advance + ellipsis.width <= (int64_t)max_width * 64) {
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
    CHECK(out->pixels);
    const double channels[4] = { ((color >> 24) & 0xff) / 255.0, ((color >> 16) & 0xff) / 255.0,
        ((color >> 8) & 0xff) / 255.0, (color & 0xff) / 255.0 };
    int64_t pen = 0;
    legacy_draw_glyphs(face, &line, keep, &pen, ascender, channels, out);
    if (ellipsis.buffer) {
        legacy_draw_glyphs(face, &ellipsis, ellipsis.count, &pen, ascender, channels, out);
        hb_buffer_destroy(ellipsis.buffer);
    }
    hb_buffer_destroy(line.buffer);
}

// ---- Measuring images ----

static int alpha_at(const struct vela_image *image, int x, int y)
{
    if (x < 0 || y < 0 || x >= image->width || y >= image->height) {
        return 0;
    }
    return (int)(image->pixels[(size_t)y * (size_t)image->width + (size_t)x] >> 24);
}

static long ink(const struct vela_image *image)
{
    long sum = 0;
    for (int y = 0; y < image->height; ++y) {
        for (int x = 0; x < image->width; ++x) {
            sum += alpha_at(image, x, y);
        }
    }
    return sum;
}

static bool same_image(const struct vela_image *a, const struct vela_image *b)
{
    return a->width == b->width && a->height == b->height
        && memcmp(a->pixels, b->pixels, (size_t)a->width * (size_t)a->height * sizeof(uint32_t)) == 0;
}

static bool same_layout(const struct vela_text_layout *a, const struct vela_text_layout *b)
{
    return a->line_x == b->line_x && a->line_y == b->line_y && a->line_width == b->line_width
        && a->line_height == b->line_height && a->ascender == b->ascender && a->fit_min == b->fit_min
        && a->fit_max == b->fit_max;
}

// The image's alpha, dark on light, for looking at the two renderers.
static void capture(const char *name, const struct vela_image *image)
{
    if (!capture_dir) {
        return;
    }
    char path[PATH_MAX];
    CHECK(snprintf(path, sizeof(path), "%s/%s.pgm", capture_dir, name) < (int)sizeof(path));
    FILE *file = fopen(path, "wb");
    CHECK(file);
    fprintf(file, "P5\n%d %d\n255\n", image->width, image->height);
    for (int y = 0; y < image->height; ++y) {
        for (int x = 0; x < image->width; ++x) {
            fputc(255 - alpha_at(image, x, y), file);
        }
    }
    CHECK(fclose(file) == 0);
}

// The centers of the ink's columns between empty columns, in pixels from the
// image's left edge (a column's center is x + 0.5). Returns how many.
static int ink_centers(const struct vela_image *image, double *centers, int capacity)
{
    int found = 0;
    double weight = 0.0;
    double moment = 0.0;
    for (int x = 0; x <= image->width; ++x) {
        long column = 0;
        for (int y = 0; x < image->width && y < image->height; ++y) {
            column += alpha_at(image, x, y);
        }
        if (column > 0) {
            weight += (double)column;
            moment += (double)column * (x + 0.5);
        } else if (weight > 0.0) {
            CHECK(found < capacity);
            centers[found++] = moment / weight;
            weight = 0.0;
            moment = 0.0;
        }
    }
    return found;
}

// Where HarfBuzz puts the pen for each glyph that isn't a space, in pixels
// from the line's start. Returns how many.
static int glyph_origins(struct font *font, const char *utf8, double pixel_size, double *origins, int capacity)
{
    set_size(font, pixel_size);
    struct legacy_shaped line = legacy_shape(font->hb, utf8);
    int found = 0;
    int64_t pen = 0;
    hb_codepoint_t space = 0;
    CHECK(hb_font_get_nominal_glyph(font->hb, ' ', &space));
    for (unsigned i = 0; i < line.count; ++i) {
        if (line.infos[i].codepoint != space) {
            CHECK(found < capacity);
            origins[found++] = (pen + line.positions[i].x_offset) / 64.0;
        }
        pen += line.positions[i].x_advance;
    }
    hb_buffer_destroy(line.buffer);
    return found;
}

struct errors {
    double max;
    double squares;
    int count;
};

static void add_error(struct errors *errors, double error)
{
    errors->max = fmax(errors->max, fabs(error));
    errors->squares += error * error;
    ++errors->count;
}

static double rms(const struct errors *errors)
{
    return errors->count ? sqrt(errors->squares / errors->count) : 0.0;
}

// ---- Positions ----

// A glyph repeated with spaces between: every copy is the same shape, so each
// copy's ink must move from the first one's exactly as far as HarfBuzz moved
// the pen. The spaces leave empty columns between them, and the sizes put the
// copies at many fractions of a pixel. Round letters: the center of their
// pixels is the center of their shape to a hundredth of a pixel, wherever
// they fall. A stem's isn't (a 1.3 px stem's pixel center is up to 0.08 px
// off, depending on where it falls), which would measure the pixel grid
// rather than the renderer.
static void positions_follow_harfbuzz(void)
{
    static const char *const strings[] = {
        "o o o o o o o o o o o o o o o o",
        "e e e e e e e e e e e e e e e e",
        "s s s s s s s s s s s s s s s s",
        "c c c c c c c c c c c c c c c c",
        "O O O O O O O O O O O O O O O O",
        "C C C C C C C C C C C C C C C C",
        "S S S S S S S S S S S S S S S S",
        "G G G G G G G G G G G G G G G G",
        "8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8",
    };
    char regular[PATH_MAX];
    font_path(regular, sizeof(regular), "NotoSans-Regular-subset.ttf");
    struct font font = open_font(regular);
    struct errors before = { 0 };
    struct errors after = { 0 };
    for (size_t s = 0; s < COUNT(strings); ++s) {
        for (size_t z = 0; z < COUNT(scales); ++z) {
            double size = KDE_PIXELS * scales[z];
            double origins[64];
            double old_centers[64];
            double new_centers[64];
            int glyphs = glyph_origins(&font, strings[s], size, origins, 64);
            struct vela_image old_image = { 0 };
            legacy_render(&font, strings[s], size, WHITE, 0, &old_image);
            struct vela_image new_image = { 0 };
            struct vela_text_layout layout;
            vela_text_render(font.text, strings[s], size, WHITE, 0, 1, &new_image, &layout);
            CHECK(ink_centers(&old_image, old_centers, 64) == glyphs);
            CHECK(ink_centers(&new_image, new_centers, 64) == glyphs);
            for (int k = 1; k < glyphs; ++k) {
                double moved = origins[k] - origins[0];
                add_error(&before, old_centers[k] - old_centers[0] - moved);
                add_error(&after, new_centers[k] - new_centers[0] - moved);
            }
            if (z == 0 && s == 0) {
                capture("positions-before", &old_image);
                capture("positions-after", &new_image);
            }
            vela_image_finish(&old_image);
            vela_image_finish(&new_image);
        }
    }
    printf("positions, %d glyphs: before max %.3f px rms %.3f px; after max %.3f px rms %.3f px\n", after.count,
        before.max, rms(&before), after.max, rms(&after));
    // The old renderer is off by up to half a pixel: the measurement sees it.
    CHECK(before.max > 0.4 && rms(&before) > 0.2);
    // The new one is where HarfBuzz says, within what measuring the ink's
    // center allows (about 0.02 px at most on these letters).
    CHECK(after.max < 0.05 && rms(&after) < 0.015);
    CHECK(rms(&after) * 10.0 < rms(&before));
    close_font(&font);
}

// ---- Ink ----

// The ink of one glyph, rendered by FreeType at its pen (whole pixel).
static long glyph_ink(struct font *font, const char *utf8, double pixel_size)
{
    set_size(font, pixel_size);
    struct legacy_shaped line = legacy_shape(font->hb, utf8);
    CHECK(line.count == 1);
    CHECK(FT_Load_Glyph(font->face, line.infos[0].codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0);
    const FT_Bitmap *bitmap = &font->face->glyph->bitmap;
    long sum = 0;
    for (unsigned row = 0; row < bitmap->rows; ++row) {
        for (unsigned col = 0; col < bitmap->width; ++col) {
            sum += bitmap->buffer[row * (unsigned)bitmap->pitch + col];
        }
    }
    hb_buffer_destroy(line.buffer);
    return sum;
}

// Single glyphs (each at the pen, a whole pixel, so the old and the new
// renderer rasterize the same bitmap) whose ink leaves the line box: left,
// right, above, below. And ordinary ones, which must not change at all.
static void no_ink_is_cut(void)
{
    static const char *const files[] = { "NotoSans-Regular-subset.ttf", "NotoSans-Italic-subset.ttf" };
    static const char *const glyphs[] = { "j", "ʃ", "ƒ", "f", "Ǻ", "Ấ", "Ỗ", "Å", "g", "y", "Q", "W", "(", "/", "x",
        "H", "o", "…" };
    long expected_total = 0;
    long lost_before = 0;
    long lost_after = 0;
    int cut_before = 0;
    for (size_t f = 0; f < COUNT(files); ++f) {
        char path[PATH_MAX];
        font_path(path, sizeof(path), files[f]);
        struct font font = open_font(path);
        for (size_t g = 0; g < COUNT(glyphs); ++g) {
            for (size_t z = 0; z < COUNT(scales); ++z) {
                double size = KDE_PIXELS * scales[z];
                long expected = glyph_ink(&font, glyphs[g], size);
                struct vela_image old_image = { 0 };
                legacy_render(&font, glyphs[g], size, WHITE, 0, &old_image);
                struct vela_image new_image = { 0 };
                struct vela_text_layout layout;
                vela_text_render(font.text, glyphs[g], size, WHITE, 0, 1, &new_image, &layout);
                long old_ink = ink(&old_image);
                long new_ink = ink(&new_image);
                expected_total += expected;
                lost_before += expected - old_ink;
                lost_after += expected - new_ink;
                cut_before += old_ink < expected;
                // Every bit of ink, exactly (the alpha is the coverage).
                CHECK(new_ink == expected);
                // The line box is where it was: inside the old image, the
                // same pixels, so the text hasn't moved and nothing else changed.
                for (int y = 0; y < old_image.height; ++y) {
                    for (int x = 0; x < old_image.width; ++x) {
                        uint32_t old_pixel = old_image.pixels[(size_t)y * (size_t)old_image.width + (size_t)x];
                        int nx = layout.line_x + x;
                        int ny = layout.line_y + y;
                        uint32_t new_pixel = nx < new_image.width && ny < new_image.height
                            ? new_image.pixels[(size_t)ny * (size_t)new_image.width + (size_t)nx]
                            : 0;
                        CHECK(old_pixel == new_pixel);
                    }
                }
                CHECK(layout.line_height == old_image.height);
                if (z == COUNT(scales) - 1 && (g == 0 || g == 1)) {
                    char name[64];
                    snprintf(name, sizeof(name), "ink-%zu-%zu-before", f, g);
                    capture(name, &old_image);
                    snprintf(name, sizeof(name), "ink-%zu-%zu-after", f, g);
                    capture(name, &new_image);
                }
                vela_image_finish(&old_image);
                vela_image_finish(&new_image);
            }
        }
        close_font(&font);
    }
    printf("ink, %zu glyphs: before %d cut, %.2f%% of the ink lost; after %.2f%% lost\n",
        COUNT(files) * COUNT(glyphs) * COUNT(scales), cut_before, 100.0 * lost_before / expected_total,
        100.0 * lost_after / expected_total);
    // The old renderer cuts these glyphs (the measurement sees it); the new
    // one cuts none.
    CHECK(cut_before >= 30 && lost_before > 0);
    CHECK(lost_after == 0);
}

// A title starting with "j" in KDE's default font, as a terminal or an editor
// shows it: the tail is cut off before, there after. Glyphs at fractional
// positions draw different pixels, so only the ink is compared: with the
// first glyph at the pen, the new image has what the old one had plus the
// tail.
static void title_starting_with_j(void)
{
    char regular[PATH_MAX];
    font_path(regular, sizeof(regular), "NotoSans-Regular-subset.ttf");
    struct font font = open_font(regular);
    for (size_t z = 0; z < COUNT(scales); ++z) {
        double size = KDE_PIXELS * scales[z];
        struct vela_image old_image = { 0 };
        legacy_render(&font, "jupyter-lab", size, WHITE, 0, &old_image);
        struct vela_image new_image = { 0 };
        struct vela_text_layout layout;
        vela_text_render(font.text, "jupyter-lab", size, WHITE, 0, 1, &new_image, &layout);
        // The tail, left of the pen: missing before, there now.
        long tail = 0;
        for (int y = 0; y < new_image.height; ++y) {
            for (int x = 0; x < layout.line_x; ++x) {
                tail += alpha_at(&new_image, x, y);
            }
        }
        CHECK(layout.line_x >= 1 && tail > 0);
        long old_left = 0;
        for (int y = 0; y < old_image.height; ++y) {
            old_left += alpha_at(&old_image, 0, y);
        }
        // The old image's first column has ink: the cut went through the tail.
        CHECK(old_left > 0);
        if (z == 0) {
            capture("title-j-before", &old_image);
            capture("title-j-after", &new_image);
        }
        vela_image_finish(&old_image);
        vela_image_finish(&new_image);
    }
    close_font(&font);
}

// Marks stacked above a letter (an app can send any title): above the line
// box, cut off before, there now.
static void stacked_marks(void)
{
    char regular[PATH_MAX];
    font_path(regular, sizeof(regular), "NotoSans-Regular-subset.ttf");
    struct font font = open_font(regular);
    for (size_t z = 0; z < COUNT(scales); ++z) {
        double size = KDE_PIXELS * scales[z];
        struct vela_image old_image = { 0 };
        legacy_render(&font, STACKED, size, WHITE, 0, &old_image);
        struct vela_image new_image = { 0 };
        struct vela_text_layout layout;
        vela_text_render(font.text, STACKED, size, WHITE, 0, 1, &new_image, &layout);
        long above = 0;
        for (int y = 0; y < layout.line_y; ++y) {
            for (int x = 0; x < new_image.width; ++x) {
                above += alpha_at(&new_image, x, y);
            }
        }
        long old_top = 0;
        for (int x = 0; x < old_image.width; ++x) {
            old_top += alpha_at(&old_image, x, 0);
        }
        CHECK(layout.line_y >= 1 && above > 0);
        CHECK(old_top > 0); // the old image's top row has ink: the cut went through the marks
        CHECK(ink(&new_image) > ink(&old_image));
        if (z == COUNT(scales) - 1) {
            capture("marks-before", &old_image);
            capture("marks-after", &new_image);
        }
        vela_image_finish(&old_image);
        vela_image_finish(&new_image);
    }
    close_font(&font);
}

// An app can send any title: forty marks stacked on one letter would reach
// far above the bar. The image grows by at most an em (plus a pixel) on each
// side, so it stays small, and the ink within that reach is drawn.
static void stacked_marks_are_bounded(void)
{
    char title[256] = "x";
    for (int i = 0; i < 40; ++i) {
        strcat(title, "\u0301");
    }
    char regular[PATH_MAX];
    font_path(regular, sizeof(regular), "NotoSans-Regular-subset.ttf");
    struct font font = open_font(regular);
    for (size_t z = 0; z < COUNT(scales); ++z) {
        double size = KDE_PIXELS * scales[z];
        int reach = (int)ceil(size) + 1;
        struct vela_image image = { 0 };
        struct vela_text_layout layout;
        vela_text_render(font.text, title, size, WHITE, 0, 1, &image, &layout);
        CHECK(layout.line_y == reach); // the marks reach past it: cut at the bound
        CHECK(image.height <= layout.line_height + 2 * reach);
        CHECK(image.width <= layout.line_width + 2 * reach);
        long above = 0;
        for (int y = 0; y < layout.line_y; ++y) {
            for (int x = 0; x < image.width; ++x) {
                above += alpha_at(&image, x, y);
            }
        }
        CHECK(above > 0); // the marks within reach are drawn
        vela_image_finish(&image);
    }
    close_font(&font);
}

// ---- Margins ----

static void margins_are_whole_steps(void)
{
    static const int margins[] = { 1, 2, 3, 5, 7, 9 };
    char italic[PATH_MAX];
    font_path(italic, sizeof(italic), "NotoSans-Italic-subset.ttf");
    struct font font = open_font(italic);
    for (size_t z = 0; z < COUNT(scales); ++z) {
        double size = KDE_PIXELS * scales[z];
        struct vela_image first = { 0 };
        struct vela_text_layout first_layout;
        vela_text_render(font.text, MARGIN_TEXT, size, WHITE, 0, 1, &first, &first_layout);
        // With a margin of 1, exactly the ink's overflow.
        CHECK(first_layout.line_x > 0 && first_layout.line_y > 0);
        for (size_t m = 0; m < COUNT(margins); ++m) {
            struct vela_image image = { 0 };
            struct vela_text_layout layout;
            vela_text_render(font.text, MARGIN_TEXT, size, WHITE, 0, margins[m], &image, &layout);
            CHECK(layout.line_x % margins[m] == 0 && layout.line_y % margins[m] == 0);
            // The smallest whole number of steps that holds the ink.
            CHECK(layout.line_x >= first_layout.line_x && layout.line_x < first_layout.line_x + margins[m]);
            CHECK(layout.line_y >= first_layout.line_y && layout.line_y < first_layout.line_y + margins[m]);
            CHECK(layout.line_width == first_layout.line_width && layout.line_height == first_layout.line_height);
            CHECK(image.width - layout.line_x == first.width - first_layout.line_x);
            CHECK(image.height - layout.line_y == first.height - first_layout.line_y);
            // The same ink, at the same place relative to the line box; the
            // margin beyond the ink is empty.
            for (int y = 0; y < image.height; ++y) {
                for (int x = 0; x < image.width; ++x) {
                    int fx = x - layout.line_x + first_layout.line_x;
                    int fy = y - layout.line_y + first_layout.line_y;
                    CHECK(alpha_at(&image, x, y) == alpha_at(&first, fx, fy));
                }
            }
            CHECK(ink(&image) == ink(&first));
            vela_image_finish(&image);
        }
        vela_image_finish(&first);
    }
    close_font(&font);
}

// ---- Resizing ----

static void resizing_keeps_the_image(void)
{
    static const char *const titles[] = {
        "vela-pattern",
        "Firefox — Mozilla Firefox Private Browsing",
        "jungle book.txt — ~/Documents — Visual Studio Code",
    };
    char regular[PATH_MAX];
    font_path(regular, sizeof(regular), "NotoSans-Regular-subset.ttf");
    struct font font = open_font(regular);
    long frames = 0;
    long drawn_before = 0;
    long drawn_after = 0;
    long ordinary_frames = 0;
    long ordinary_drawn = 0;
    for (size_t t = 0; t < COUNT(titles); ++t) {
        for (size_t z = 0; z < COUNT(scales); ++z) {
            double size = KDE_PIXELS * scales[z];
            // An interactive resize through every width, as decoration.c
            // sees it: one width per frame, from cut to the first glyph to
            // whole with room to spare.
            struct vela_image whole = { 0 };
            struct vela_text_layout whole_layout;
            vela_text_render(font.text, titles[t], size, WHITE, 0, 1, &whole, &whole_layout);
            CHECK(whole_layout.fit_max == INT_MAX && vela_text_fits(&whole_layout, 0));
            int widest = whole_layout.fit_min + 64;
            struct vela_image kept = { 0 };
            struct vela_text_layout kept_layout = { 0 };
            bool have = false;
            // Narrowed from the widest to 1, then widened back: each bound
            // of the range is crossed in the direction that would show a
            // stale title if it were wrong.
            for (int frame = 0; frame < 2 * widest - 1; ++frame) {
                int width = frame < widest ? widest - frame : frame - widest + 2;
                struct vela_image image = { 0 };
                struct vela_text_layout layout;
                vela_text_render(font.text, titles[t], size, WHITE, width, 1, &image, &layout);
                ++frames;
                ++drawn_before; // the old decoration.c drew the title on every new width
                CHECK(vela_text_fits(&layout, width));
                if (have && vela_text_fits(&kept_layout, width)) {
                    // Kept: it must be exactly what drawing it again makes.
                    CHECK(same_image(&kept, &image) && same_layout(&kept_layout, &layout));
                } else {
                    // Drawn again: it must have changed (the range was as wide
                    // as it could be) unless it is the first.
                    if (have) {
                        CHECK(!same_image(&kept, &image));
                        CHECK(
                            width == (long long)kept_layout.fit_max + 1 || width == (long long)kept_layout.fit_min - 1);
                    }
                    ++drawn_after;
                    vela_image_finish(&kept);
                    kept = image;
                    kept_layout = layout;
                    have = true;
                    continue;
                }
                vela_image_finish(&image);
            }
            // Back to the widest: the same image as without a limit.
            CHECK(same_image(&kept, &whole));
            CHECK(!vela_text_fits(&kept_layout, -1));
            // The usual case: a window dragged 600 pixels wider and back,
            // with room for the title all along. Drawn on every frame before,
            // never again now, and every frame would draw this same image.
            for (int step = 0; step < 1200; ++step) {
                int width = whole_layout.fit_min + 20 + (step < 600 ? step : 1199 - step);
                ++ordinary_frames;
                if (!vela_text_fits(&kept_layout, width)) {
                    ++ordinary_drawn;
                }
                if (step % 97 == 0) {
                    struct vela_image image = { 0 };
                    struct vela_text_layout layout;
                    vela_text_render(font.text, titles[t], size, WHITE, width, 1, &image, &layout);
                    CHECK(same_image(&kept, &image) && same_layout(&kept_layout, &layout));
                    vela_image_finish(&image);
                }
            }
            vela_image_finish(&kept);
            vela_image_finish(&whole);
        }
    }
    printf("resizing through every width, %ld frames: before %ld titles drawn, after %ld (one per image)\n", frames,
        drawn_before, drawn_after);
    printf("resizing with room for the title, %ld frames: before %ld titles drawn, after %ld\n", ordinary_frames,
        ordinary_frames, ordinary_drawn);
    // Through every width the new count is the least possible (each draw
    // makes a different image, checked above); the old one drew every frame.
    CHECK(drawn_after * 8 < drawn_before);
    CHECK(ordinary_drawn == 0);
    close_font(&font);
}

// ---- Bitmap glyphs ----

// A bitmap font (BDF, 13 px): "A" and "B" in 8x13 cells, 1 bit per pixel.
static const char bdf[] = "STARTFONT 2.1\n"
                          "FONT -vela-test-medium-r-normal--13-130-75-75-c-80-iso10646-1\n"
                          "SIZE 13 75 75\n"
                          "FONTBOUNDINGBOX 8 13 0 -3\n"
                          "STARTPROPERTIES 3\n"
                          "FONT_ASCENT 10\n"
                          "FONT_DESCENT 3\n"
                          "PIXEL_SIZE 13\n"
                          "ENDPROPERTIES\n"
                          "CHARS 2\n"
                          "STARTCHAR A\nENCODING 65\nSWIDTH 615 0\nDWIDTH 8 0\nBBX 8 13 0 -3\nBITMAP\n"
                          "00\n18\n24\n42\n42\n7E\n42\n42\n42\n00\n00\n00\n00\nENDCHAR\n"
                          "STARTCHAR B\nENCODING 66\nSWIDTH 615 0\nDWIDTH 8 0\nBBX 8 13 0 -3\nBITMAP\n"
                          "00\n7C\n42\n42\n7C\n42\n42\n42\n7C\n00\n00\n00\n00\nENDCHAR\n"
                          "ENDFONT\n";

static int set_bits(void)
{
    static const unsigned rows[] = { 0x18, 0x24, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x42, 0x7C, 0x42, 0x42, 0x7C, 0x42, 0x42,
        0x42, 0x7C };
    int bits = 0;
    for (size_t i = 0; i < COUNT(rows); ++i) {
        bits += __builtin_popcount(rows[i]);
    }
    return bits;
}

// Pixels that are neither empty nor full: a 1-bit glyph has none.
static int partial_pixels(const struct vela_image *image)
{
    int partial = 0;
    for (int y = 0; y < image->height; ++y) {
        for (int x = 0; x < image->width; ++x) {
            int alpha = alpha_at(image, x, y);
            partial += alpha != 0 && alpha != 255;
        }
    }
    return partial;
}

static void bitmap_glyphs_are_exact(void)
{
    // A private directory under the test's own working directory (the build
    // tree), never in a publicly writable one.
    char cwd[PATH_MAX - 64];
    CHECK(getcwd(cwd, sizeof(cwd)));
    char directory[PATH_MAX];
    CHECK(snprintf(directory, sizeof(directory), "%s/vela-text-XXXXXX", cwd) < (int)sizeof(directory));
    CHECK(mkdtemp(directory));
    char path[PATH_MAX];
    CHECK(snprintf(path, sizeof(path), "%s/test.bdf", directory) < (int)sizeof(path));
    FILE *file = fopen(path, "w");
    CHECK(file);
    CHECK(fputs(bdf, file) >= 0);
    CHECK(fclose(file) == 0);
    struct font font = open_font(path);

    // Before: the old renderer read `width` bytes from each row of a glyph's
    // bitmap. A 1-bit row is `pitch` bytes for `width` pixels, so it read
    // bits as coverage and, on the last rows, past the end of FreeType's
    // buffer (which is why legacy_render isn't run on it: under the
    // sanitizers that read is an error).
    set_size(&font, 13.0);
    CHECK(FT_Load_Glyph(font.face, FT_Get_Char_Index(font.face, 'A'), FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0);
    const FT_Bitmap *bitmap = &font.face->glyph->bitmap;
    CHECK(bitmap->pixel_mode == FT_PIXEL_MODE_MONO);
    size_t last_read = (size_t)(bitmap->rows - 1) * (size_t)bitmap->pitch + bitmap->width;
    CHECK(last_read > (size_t)bitmap->rows * (size_t)bitmap->pitch);

    // After: converted, every set bit is a full pixel and nothing else has ink.
    struct vela_image image = { 0 };
    struct vela_text_layout layout;
    vela_text_render(font.text, "AB", 13.0, WHITE, 0, 1, &image, &layout);
    int bits = set_bits();
    printf(
        "bitmap font, %d pixels set: before, reads %zu bytes of a %zu-byte bitmap; after, %ld px of ink, %d partial\n",
        bits, last_read, (size_t)bitmap->rows * (size_t)bitmap->pitch, ink(&image) / 255, partial_pixels(&image));
    CHECK(ink(&image) == (long)bits * 255 && partial_pixels(&image) == 0);
    // At its cell, by whole pixels: "A"'s top row (0x18) is the cell's second
    // row, the cell starting at the ascender (10 rows above the baseline);
    // "B" (0x7C) starts 8 pixels on.
    int baseline = layout.line_y + layout.ascender;
    CHECK(layout.ascender == 10);
    CHECK(alpha_at(&image, layout.line_x + 3, baseline - 9) == 255);
    CHECK(alpha_at(&image, layout.line_x + 4, baseline - 9) == 255);
    CHECK(alpha_at(&image, layout.line_x + 2, baseline - 9) == 0);
    CHECK(alpha_at(&image, layout.line_x + 5, baseline - 9) == 0);
    CHECK(alpha_at(&image, layout.line_x + 8 + 1, baseline - 9) == 255);
    CHECK(alpha_at(&image, layout.line_x + 8 + 0, baseline - 9) == 0);
    capture("bitmap-after", &image);
    vela_image_finish(&image);
    close_font(&font);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
}

// ---- Loading the title font ----

static void missing_font(void)
{
    char path[PATH_MAX];
    font_path(path, sizeof(path), "missing.ttf");
    CHECK(vela_text_create_file(path, 0, 10.0) == NULL);
}

static void loads_configured_title_font(void)
{
    char cwd[PATH_MAX - 64];
    CHECK(getcwd(cwd, sizeof(cwd)));
    char directory[PATH_MAX];
    CHECK(snprintf(directory, sizeof(directory), "%s/vela-font-config-XXXXXX", cwd) < (int)sizeof(directory));
    CHECK(mkdtemp(directory));
    CHECK(setenv("XDG_CONFIG_HOME", directory, 1) == 0);

    char kdeglobals[PATH_MAX];
    CHECK(snprintf(kdeglobals, sizeof(kdeglobals), "%s/kdeglobals", directory) < (int)sizeof(kdeglobals));
    FILE *file = fopen(kdeglobals, "w");
    CHECK(file);
    CHECK(fputs("[General]\nfont=Noto Sans,18\n[WM]\nactiveFont=Noto Sans,12\n", file) >= 0);
    CHECK(fclose(file) == 0);

    // Keep font matching independent of the machine's fonts and settings.
    FcConfig *config = FcConfigCreate();
    CHECK(config && FcConfigSetCurrent(config));
    CHECK(vela_text_create() == NULL);

    char path[PATH_MAX];
    font_path(path, sizeof(path), "NotoSans-Regular-subset.ttf");
    CHECK(FcConfigAppFontAddFile(config, (const FcChar8 *)path));
    struct vela_text *text = vela_text_create();
    CHECK(text && vela_text_pixel_size(text) == 16.0);
    struct vela_text *bundled = vela_text_create_file(path, 0, 12.0);
    CHECK(bundled);

    struct vela_image actual = { 0 };
    struct vela_image expected = { 0 };
    struct vela_text_layout actual_layout;
    struct vela_text_layout expected_layout;
    vela_text_render(text, "jupyter", vela_text_pixel_size(text), WHITE, 0, 1, &actual, &actual_layout);
    vela_text_render(bundled, "jupyter", 16.0, WHITE, 0, 1, &expected, &expected_layout);
    CHECK(ink(&actual) > 0 && same_image(&actual, &expected) && same_layout(&actual_layout, &expected_layout));

    vela_image_finish(&actual);
    vela_image_finish(&expected);
    vela_text_destroy(bundled);
    vela_text_destroy(text);
    FcConfigDestroy(config);
    FcFini();
    CHECK(unlink(kdeglobals) == 0);
    CHECK(rmdir(directory) == 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s FONTS_DIR [--capture-dir DIR]\n", argv[0]);
        return EXIT_FAILURE;
    }
    CHECK(snprintf(fonts, sizeof(fonts), "%s", argv[1]) < (int)sizeof(fonts));
    if (argc >= 4 && strcmp(argv[2], "--capture-dir") == 0) {
        capture_dir = argv[3];
        CHECK(mkdir(capture_dir, 0700) == 0 || errno == EEXIST);
    }
    CHECK(FT_Init_FreeType(&library) == 0);
    positions_follow_harfbuzz();
    no_ink_is_cut();
    title_starting_with_j();
    stacked_marks();
    stacked_marks_are_bounded();
    margins_are_whole_steps();
    resizing_keeps_the_image();
    bitmap_glyphs_are_exact();
    missing_font();
    loads_configured_title_font();
    FT_Done_FreeType(library);
    puts("PASS");
    return EXIT_SUCCESS;
}
