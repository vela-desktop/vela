// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "decoration.h"

#include "icons.h"
#include "motion.h"
#include "render/pixel_buffer.h"
#include "scene/scene.h"
#include "server.h"
#include "text.h"
#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>

#define HEIGHT VELA_DECORATION_HEIGHT
#define BUTTON_WIDTH VELA_DECORATION_BUTTON_WIDTH
#define GLYPH_SIZE 10 // logical, like Windows 11's glyphs
#define TITLE_MARGIN 12 // without an icon
#define TITLE_AFTER_ICON (VELA_DECORATION_ICON_X + VELA_DECORATION_ICON_SIZE + 8)

// The buttons' hover: WinUI's SubtleFillColorSecondary, Theme.subtleHover in
// QML (white in dark mode, black in light mode; scripts/check-shared-values.py
// keeps them equal).
#define HOVER_DARK_ALPHA 0.061f
#define HOVER_LIGHT_ALPHA 0.037f

// Colors like Windows 11, in dark and light theme (the apps' mode). The
// backgrounds without Mica (no shell, or before it sends the colors).
struct palette {
    struct wlr_render_color active_background;
    struct wlr_render_color inactive_background;
    struct wlr_render_color hover; // premultiplied
    uint32_t active_text;
    uint32_t inactive_text;
};

static const struct palette dark_palette = {
    { 0.125f, 0.125f, 0.125f, 1.0f }, // #202020
    { 0.169f, 0.169f, 0.169f, 1.0f }, // #2b2b2b
    { HOVER_DARK_ALPHA, HOVER_DARK_ALPHA, HOVER_DARK_ALPHA, HOVER_DARK_ALPHA },
    0xffffffff,
    0xff9a9a9a,
};

static const struct palette light_palette = {
    { 0.953f, 0.953f, 0.953f, 1.0f }, // #f3f3f3
    { 0.976f, 0.976f, 0.976f, 1.0f }, // #f9f9f9
    { 0.0f, 0.0f, 0.0f, HOVER_LIGHT_ALPHA },
    0xff1a1a1a,
    0xff8f8f8f,
};

// Close's red is its hover; on it the glyph is white.
static const struct wlr_render_color close_hover_color = { 0.769f, 0.169f, 0.110f, 1.0f }; // #c42b1c
#define CLOSE_HOVER_TEXT 0xffffffffu

static const enum vela_decoration_part button_parts[3] = {
    VELA_DECORATION_MINIMIZE,
    VELA_DECORATION_MAXIMIZE,
    VELA_DECORATION_CLOSE,
};

// A CPU image (text, glyph) as a scene node.
struct image {
    struct vela_buffer_node *node;
};

// A button's hover, faded (docs/visual-coherence.md §6.6): `amount` goes from
// 0 (none) to 1 (its hover color), retargeted from where it is.
struct fade {
    double from, to, amount;
    struct vela_tween tween;
    bool running;
};

struct vela_decoration {
    struct vela_server *server;
    struct vela_tree *tree;
    struct vela_rect_node *background;
    struct vela_rect_node *hover_rects[3]; // minimize, maximize, close
    struct fade fades[3];
    struct image glyphs[3];
    struct image close_white; // above Close's glyph, faded with its red
    struct image title;
    struct image icon;

    // What is drawn now.
    int width;
    float scale;
    char *title_text;
    char *app_id;
    bool has_icon;
    uint32_t look_version; // the theme and Mica it's drawn with
    uint32_t text_color; // the title's and the glyphs'
    bool active;
    bool maximized;
    bool can_maximize;
    bool drawn;
    enum vela_decoration_part hover;
};

static const struct palette *palette(const struct vela_server *server)
{
    return server->light_apps ? &light_palette : &dark_palette;
}

// ----------------------------------------------------------------- glyphs --

struct segment {
    double x1, y1, x2, y2;
};

static const struct segment minimize_glyph[] = { { 0.0, 0.5, 1.0, 0.5 } };
static const struct segment maximize_glyph[] = {
    { 0.0, 0.0, 1.0, 0.0 },
    { 1.0, 0.0, 1.0, 1.0 },
    { 1.0, 1.0, 0.0, 1.0 },
    { 0.0, 1.0, 0.0, 0.0 },
};
// "Restore": two overlapping squares.
static const struct segment restore_glyph[] = {
    { 0.0, 0.2, 0.8, 0.2 },
    { 0.8, 0.2, 0.8, 1.0 },
    { 0.8, 1.0, 0.0, 1.0 },
    { 0.0, 1.0, 0.0, 0.2 },
    { 0.2, 0.2, 0.2, 0.0 },
    { 0.2, 0.0, 1.0, 0.0 },
    { 1.0, 0.0, 1.0, 0.8 },
    { 1.0, 0.8, 0.8, 0.8 },
};
static const struct segment close_glyph[] = { { 0.0, 0.0, 1.0, 1.0 }, { 1.0, 0.0, 0.0, 1.0 } };

static const struct segment *glyph(enum vela_decoration_part part, bool maximized, int *count)
{
    switch (part) {
    case VELA_DECORATION_MINIMIZE:
        *count = 1;
        return minimize_glyph;
    case VELA_DECORATION_MAXIMIZE:
        *count = maximized ? 8 : 4;
        return maximized ? restore_glyph : maximize_glyph;
    case VELA_DECORATION_CLOSE:
        *count = 2;
        return close_glyph;
    default:
        *count = 0;
        return NULL;
    }
}

static uint32_t scaled_channel(uint32_t color, int shift, double coverage)
{
    return (uint32_t)lround(((color >> shift) & 0xff) * coverage) << shift;
}

// A glyph made of segments in the unit square, drawn at `size` pixels with a
// `stroke` pixel line and antialiased edges, premultiplied.
static struct vela_image draw_segments(int size, double stroke, uint32_t color, const struct segment *segments,
    int count)
{
    struct vela_image image = { size, size, calloc((size_t)size * (size_t)size, 4) };
    double span = size - stroke; // the stroke stays inside the image
    double offset = stroke / 2.0;
    double alpha = ((color >> 24) & 0xff) / 255.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            double px = x + 0.5;
            double py = y + 0.5;
            double distance = 1e9;
            for (int i = 0; i < count; ++i) {
                const struct segment *s = &segments[i];
                double ax = offset + s->x1 * span;
                double ay = offset + s->y1 * span;
                double bx = offset + s->x2 * span;
                double by = offset + s->y2 * span;
                double dx = bx - ax;
                double dy = by - ay;
                double length = dx * dx + dy * dy;
                double t = length > 0 ? vela_clampd(((px - ax) * dx + (py - ay) * dy) / length, 0.0, 1.0) : 0.0;
                distance = fmin(distance, hypot(px - (ax + t * dx), py - (ay + t * dy)));
            }
            double coverage = vela_clampd(stroke / 2.0 + 0.5 - distance, 0.0, 1.0) * alpha;
            if (coverage > 0.0) {
                image.pixels[(size_t)y * (size_t)size + (size_t)x] = (uint32_t)lround(coverage * 255.0) << 24
                    | scaled_channel(color, 16, coverage) | scaled_channel(color, 8, coverage)
                    | scaled_channel(color, 0, coverage);
            }
        }
    }
    return image;
}

static struct vela_image draw_glyph(enum vela_decoration_part part, bool maximized, float scale, uint32_t color)
{
    int size = vela_max(1, (int)lround(GLYPH_SIZE * scale));
    double stroke = fmax(1.0, scale);
    int count = 0;
    const struct segment *segments = glyph(part, maximized, &count);
    return draw_segments(size, stroke, color, segments, count);
}

// ---------------------------------------------------------------- colors --

// The bar's background: Mica as the shell computed it (docs/renderer.md
// §9.4), or Windows 11's gray.
static struct wlr_render_color background_color(const struct vela_server *server, bool active)
{
    const struct palette *colors = palette(server);
    if (!server->has_mica) {
        return active ? colors->active_background : colors->inactive_background;
    }
    return active ? server->mica_active : server->mica_inactive;
}

// A button's hover color at `amount` (premultiplied: a scaled color fades).
static struct wlr_render_color hover_color(const struct vela_server *server, int button, double amount)
{
    struct wlr_render_color full
        = button_parts[button] == VELA_DECORATION_CLOSE ? close_hover_color : palette(server)->hover;
    float a = (float)amount;
    return (struct wlr_render_color) { full.r * a, full.g * a, full.b * a, full.a * a };
}

static void apply_fade(struct vela_decoration *decoration, int button)
{
    struct wlr_render_color color = hover_color(decoration->server, button, decoration->fades[button].amount);
    vela_rect_node_set_color(decoration->hover_rects[button], &color);
    if (button_parts[button] == VELA_DECORATION_CLOSE && decoration->close_white.node) {
        vela_node_set_opacity(&decoration->close_white.node->node, (float)decoration->fades[button].amount);
    }
}

// ----------------------------------------------------------------- images --

// The texture of a bar image lives as long as its buffer, not the image: a
// snapshot (window animations) can keep the buffer locked after the bar has
// replaced it.
struct texture_owner {
    struct wlr_addon addon;
    struct wlr_texture *texture;
};

static void destroy_texture_owner(struct wlr_addon *addon)
{
    struct texture_owner *owner = wl_container_of(addon, owner, addon);
    wlr_texture_destroy(owner->texture);
    wlr_addon_finish(addon);
    free(owner);
}

static const struct wlr_addon_interface texture_owner_impl = {
    .name = "vela-decoration-texture",
    .destroy = destroy_texture_owner,
};

static void clear_image(struct image *image)
{
    // Unlocks the buffer; the texture goes with it (texture_owner).
    if (image->node) {
        vela_node_destroy(&image->node->node);
    }
    image->node = NULL;
}

// Takes over the pixels of `pixels`.
static void set_image(struct vela_decoration *decoration, struct image *image, struct vela_image *pixels, double x,
    double y, double logical_width, double logical_height)
{
    clear_image(image);
    int width = pixels->width;
    int height = pixels->height;
    struct wlr_buffer *buffer = vela_pixel_buffer_create(pixels);
    struct wlr_texture *texture = wlr_texture_from_buffer(decoration->server->wlr_renderer, buffer);
    if (texture) {
        struct texture_owner *owner = calloc(1, sizeof(*owner));
        owner->texture = texture;
        wlr_addon_init(&owner->addon, &buffer->addons, owner, &texture_owner_impl);
        const struct wlr_fbox src = { 0, 0, width, height };
        image->node = vela_buffer_node_create(decoration->tree, buffer, texture, &src,
            WL_OUTPUT_TRANSFORM_NORMAL, logical_width, logical_height);
        vela_node_set_position(&image->node->node, x, y);
        image->node->node.hittable = true;
    }
    wlr_buffer_drop(buffer);
}

// The bars' text, loaded on first use (NULL: no font).
static struct vela_text *text_engine(struct vela_server *server)
{
    if (!server->text_loaded) {
        server->text_loaded = true;
        server->text = vela_text_create();
    }
    return server->text;
}

static struct vela_icons *icon_loader(struct vela_server *server)
{
    if (!server->icons) {
        server->icons = vela_icons_create();
    }
    return server->icons;
}

static void layout_buttons(struct vela_decoration *decoration)
{
    for (int i = 0; i < 3; ++i) {
        double x = decoration->width - (3 - i) * BUTTON_WIDTH;
        vela_node_set_position(&decoration->hover_rects[i]->node, x, 0);
        if (decoration->glyphs[i].node) {
            vela_node_set_position(&decoration->glyphs[i].node->node, x + (BUTTON_WIDTH - GLYPH_SIZE) / 2.0,
                (HEIGHT - GLYPH_SIZE) / 2.0);
        }
    }
    if (decoration->close_white.node) {
        double x = decoration->width - BUTTON_WIDTH;
        vela_node_set_position(&decoration->close_white.node->node, x + (BUTTON_WIDTH - GLYPH_SIZE) / 2.0,
            (HEIGHT - GLYPH_SIZE) / 2.0);
    }
}

static bool same_text(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

// -------------------------------------------------------------------- bar --

void vela_decoration_update(struct vela_decoration *decoration, const struct vela_decoration_state *state)
{
    struct vela_server *server = decoration->server;
    vela_node_set_enabled(&decoration->tree->node, !state->fullscreen);
    // At the top of the window frame (Wayland apps can have their geometry
    // offset from the surface origin).
    vela_node_set_position(&decoration->tree->node, state->geometry.x, state->geometry.y);
    int width = state->geometry.width;
    float scale = state->scale;
    bool active = state->active;
    bool maximized = state->maximized;
    // Each image is drawn again only when what it is drawn from changes: a
    // new Mica or accent changes colors, not images.
    bool first = !decoration->drawn;
    bool resized = width != decoration->width;
    bool rescaled = first || scale != decoration->scale;
    bool reactivated = first || active != decoration->active;
    bool relooked = first || decoration->look_version != server->look_version;
    bool remaximized = first || maximized != decoration->maximized;
    bool can_maximize = state->can_maximize;
    bool resize_capability_changed = first || can_maximize != decoration->can_maximize;
    bool new_app = !same_text(state->app_id, decoration->app_id);
    bool new_title = !same_text(state->title, decoration->title_text);
    const struct palette *colors = palette(server);
    uint32_t text_color = active ? colors->active_text : colors->inactive_text;
    bool recolored = first || text_color != decoration->text_color;
    decoration->width = width;
    if (resized) {
        vela_rect_node_set_size(decoration->background, width, HEIGHT);
    }
    if (relooked || reactivated) {
        struct wlr_render_color background = background_color(server, active);
        vela_rect_node_set_color(decoration->background, &background);
    }

    // The app icon, drawn at the exact physical size (§9.6).
    if (rescaled || new_app) {
        int pixels = vela_max(1, (int)lround(VELA_DECORATION_ICON_SIZE * scale));
        const struct vela_image *icon = vela_icons_app(icon_loader(server), state->app_id, pixels);
        decoration->has_icon = icon->pixels != NULL;
        if (decoration->has_icon) {
            size_t bytes = (size_t)icon->width * (size_t)icon->height * 4;
            struct vela_image copy = { icon->width, icon->height, malloc(bytes) };
            memcpy(copy.pixels, icon->pixels, bytes);
            set_image(decoration, &decoration->icon, &copy, VELA_DECORATION_ICON_X,
                (HEIGHT - VELA_DECORATION_ICON_SIZE) / 2.0, VELA_DECORATION_ICON_SIZE, VELA_DECORATION_ICON_SIZE);
        } else {
            clear_image(&decoration->icon);
        }
    }
    if (decoration->icon.node) {
        vela_node_set_opacity(&decoration->icon.node->node, active ? 1.0f : 0.6f);
    }
    int title_x = decoration->has_icon ? TITLE_AFTER_ICON : TITLE_MARGIN;

    // The title, rasterized at the output's physical pixels.
    struct vela_text *text = text_engine(server);
    if (text && (recolored || rescaled || resized || new_title || new_app)) {
        int max_width = (int)((width - title_x - 3 * BUTTON_WIDTH - 8) * scale);
        if (max_width > 0 && state->title[0]) {
            struct vela_image image = { 0 };
            vela_text_render(text, state->title, vela_text_pixel_size(text) * scale, text_color, max_width, &image);
            double logical_height = image.height / (double)scale;
            double logical_width = image.width / (double)scale;
            set_image(decoration, &decoration->title, &image, title_x, round((HEIGHT - logical_height) / 2.0),
                logical_width, logical_height);
        } else {
            clear_image(&decoration->title);
        }
    }

    // The button glyphs in the text's color; Close's white one, shown as much
    // as its red is, only from the scale and the state.
    bool glyphs = recolored || rescaled || remaximized || resize_capability_changed;
    if (glyphs) {
        for (int i = 0; i < 3; ++i) {
            // Keep the button visible but visually disabled on fixed-size windows.
            uint32_t color = (i == 1 && !can_maximize)
                ? (text_color & 0x00ffffffu) | ((text_color >> 1) & 0x7f000000u) : text_color;
            struct vela_image pixels = draw_glyph(button_parts[i], maximized, scale, color);
            set_image(decoration, &decoration->glyphs[i], &pixels, 0, 0, GLYPH_SIZE, GLYPH_SIZE);
        }
    }
    bool close_white = rescaled || remaximized;
    if (close_white) {
        struct vela_image white = draw_glyph(VELA_DECORATION_CLOSE, maximized, scale, CLOSE_HOVER_TEXT);
        set_image(decoration, &decoration->close_white, &white, 0, 0, GLYPH_SIZE, GLYPH_SIZE);
    }
    if (glyphs || close_white || relooked) {
        for (int i = 0; i < 3; ++i) {
            apply_fade(decoration, i); // the mode's hover color, and the white glyph's opacity
        }
    }
    if (resized || glyphs || close_white) {
        layout_buttons(decoration);
    }

    decoration->scale = scale;
    decoration->text_color = text_color;
    decoration->active = active;
    decoration->maximized = maximized;
    decoration->can_maximize = can_maximize;
    if (new_title) {
        free(decoration->title_text);
        decoration->title_text = strdup(state->title);
    }
    if (new_app) {
        free(decoration->app_id);
        decoration->app_id = strdup(state->app_id);
    }
    decoration->look_version = server->look_version;
    decoration->drawn = true;
}

struct vela_decoration *vela_decoration_create(struct vela_server *server, struct vela_tree *parent,
    const struct vela_decoration_state *state)
{
    struct vela_decoration *decoration = calloc(1, sizeof(*decoration));
    decoration->server = server;
    decoration->width = -1;
    decoration->tree = vela_tree_create(parent);
    decoration->background = vela_rect_node_create(decoration->tree, 0, HEIGHT, &dark_palette.active_background);
    // Above the app's surface, outside its area.
    vela_node_set_position(&decoration->tree->node, 0, -HEIGHT);
    decoration->background->node.hittable = true;
    const struct wlr_render_color none = { 0 };
    for (int i = 0; i < 3; ++i) {
        decoration->hover_rects[i] = vela_rect_node_create(decoration->tree, BUTTON_WIDTH, HEIGHT, &none);
        decoration->hover_rects[i]->node.hittable = true;
    }
    vela_decoration_update(decoration, state);
    return decoration;
}

void vela_decoration_destroy(struct vela_decoration *decoration)
{
    if (!decoration) {
        return;
    }
    clear_image(&decoration->icon);
    clear_image(&decoration->title);
    clear_image(&decoration->close_white);
    for (int i = 0; i < 3; ++i) {
        clear_image(&decoration->glyphs[i]);
    }
    for (int i = 2; i >= 0; --i) {
        vela_node_destroy(&decoration->hover_rects[i]->node);
    }
    vela_node_destroy(&decoration->background->node);
    vela_node_destroy(&decoration->tree->node);
    free(decoration->title_text);
    free(decoration->app_id);
    free(decoration);
}

enum vela_decoration_part vela_decoration_part_at(const struct vela_decoration *decoration, double lx, double ly)
{
    double ox = 0.0;
    double oy = 0.0;
    vela_node_coords(&decoration->tree->node, &ox, &oy);
    double x = lx - ox;
    double y = ly - oy;
    int width = decoration->width;
    if (y < 0 || y >= HEIGHT || x < 0 || x >= width) {
        return VELA_DECORATION_NONE;
    }
    if (x >= width - BUTTON_WIDTH) {
        return VELA_DECORATION_CLOSE;
    }
    if (x >= width - 2 * BUTTON_WIDTH) {
        return decoration->can_maximize ? VELA_DECORATION_MAXIMIZE : VELA_DECORATION_NONE;
    }
    if (x >= width - 3 * BUTTON_WIDTH) {
        return VELA_DECORATION_MINIMIZE;
    }
    if (decoration->has_icon && x >= VELA_DECORATION_ICON_X - 6
        && x < VELA_DECORATION_ICON_X + VELA_DECORATION_ICON_SIZE + 6) {
        return VELA_DECORATION_ICON;
    }
    return VELA_DECORATION_TITLE;
}

struct wlr_box vela_decoration_maximize_box(const struct vela_decoration *decoration)
{
    double ox = 0.0;
    double oy = 0.0;
    vela_node_coords(&decoration->tree->node, &ox, &oy);
    return (struct wlr_box) { (int)lround(ox) + decoration->width - 2 * BUTTON_WIDTH, (int)lround(oy), BUTTON_WIDTH,
        HEIGHT };
}

void vela_decoration_set_hover(struct vela_decoration *decoration, enum vela_decoration_part part)
{
    if (part == decoration->hover) {
        return;
    }
    decoration->hover = part;
    // Each button fades toward its new state from where it is (a hover left
    // halfway turns back from there), in `fast`, like Vela's other hovers.
    static const char *const names[3] = { "minimize-hover", "maximize-hover", "close-hover" };
    for (int i = 0; i < 3; ++i) {
        struct fade *fade = &decoration->fades[i];
        double to = button_parts[i] == part ? 1.0 : 0.0;
        if (to == fade->to && (fade->running || fade->amount == to)) {
            continue;
        }
        fade->from = fade->amount;
        fade->to = to;
        fade->running = true;
        vela_tween_start(&fade->tween, names[i], vela_motion_ms(VELA_MOTION_FAST_MS), &vela_decelerate);
    }
    vela_server_schedule_frames(decoration->server);
}

bool vela_decoration_tick(struct vela_decoration *decoration, double now_ms, double wall_ms)
{
    bool running = false;
    for (int i = 0; i < 3; ++i) {
        struct fade *fade = &decoration->fades[i];
        if (!fade->running) {
            continue;
        }
        double p = vela_tween_progress(&fade->tween, now_ms);
        fade->amount = fade->from + (fade->to - fade->from) * p;
        if (vela_motion_logger) {
            wlr_log(WLR_INFO, "animation: %s #%u fill %.4f", fade->tween.name, fade->tween.serial, fade->amount);
        }
        if (vela_tween_retired(&fade->tween, wall_ms)) {
            fade->amount = fade->to;
            fade->running = false;
        } else {
            running = true;
        }
        apply_fade(decoration, i);
    }
    return running;
}
