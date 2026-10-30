// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_DECORATION_H
#define VELA_DECORATION_H

// The title bar Vela draws for windows without one of their own
// (docs/renderer.md §9): Windows 11 sizes (32 high, 46×32 buttons), app icon
// and title in KDE's font, glyphs drawn at the exact physical size, Mica
// colors with the wallpaper tint.
//
// The bar doesn't know the window: its owner tells it how the window is at
// every update, and it redraws only what changed.

#include <stdbool.h>
#include <wlr/util/box.h>

struct vela_server;
struct vela_tree;

#define VELA_DECORATION_HEIGHT 32 // logical
#define VELA_DECORATION_BUTTON_WIDTH 46
#define VELA_DECORATION_ICON_SIZE 16
#define VELA_DECORATION_ICON_X 12

enum vela_decoration_part {
    VELA_DECORATION_NONE,
    VELA_DECORATION_ICON,
    VELA_DECORATION_TITLE,
    VELA_DECORATION_MINIMIZE,
    VELA_DECORATION_MAXIMIZE,
    VELA_DECORATION_CLOSE,
};

// The window as it is now.
struct vela_decoration_state {
    struct wlr_box geometry; // the frame including the bar, relative to the window's tree
    float scale; // of the output it's on
    const char *title;
    const char *app_id;
    bool active;
    bool maximized;
    bool can_maximize; // false for fixed-size toplevels (including X11 size hints)
    bool fullscreen; // fullscreen hides the bar
};

struct vela_decoration;

// The bar is a tree under `parent` (the window's).
struct vela_decoration *vela_decoration_create(struct vela_server *server, struct vela_tree *parent,
    const struct vela_decoration_state *state);
void vela_decoration_destroy(struct vela_decoration *decoration);

void vela_decoration_update(struct vela_decoration *decoration, const struct vela_decoration_state *state);

// What lies at a global point of the bar.
enum vela_decoration_part vela_decoration_part_at(const struct vela_decoration *decoration, double lx, double ly);
// The button under the mouse lights up (Close in red).
void vela_decoration_set_hover(struct vela_decoration *decoration, enum vela_decoration_part part);
// The buttons' hover fades at the frame's time (docs/visual-coherence.md
// §6.2); whether any is still running.
bool vela_decoration_tick(struct vela_decoration *decoration, double now_ms, double wall_ms);
// The Maximize button in global coordinates (snap layouts open below it).
struct wlr_box vela_decoration_maximize_box(const struct vela_decoration *decoration);

#endif
