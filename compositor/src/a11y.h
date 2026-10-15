// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_A11Y_H
#define VELA_A11Y_H

// Accessibility and screen color, like Windows 11:
//
// - Night light: warmer colors in the evening, by hand (quick settings) or
//   scheduled, between two times or from sunset to sunrise (sun times come
//   from the time zone's coordinates, without network).
// - Color filters: grayscale and corrections for color blindness.
// - Magnifier: Win+plus zooms the cursor's output around it, Win+minus zooms
//   out, Win+Esc closes; the zoomed area follows the cursor at its edges.
// - Sticky keys: Shift, Ctrl, Alt and Win pressed and released apply to the
//   next key; pressed twice they stay until pressed again.
//
// The renderer applies night light and filters to everything it draws (a
// color matrix, docs/renderer.md §7); the magnifier is the scene drawn at a
// larger scale. The choices are in vela.conf; the shell turns them on from
// quick settings and receives the state ("accessibility <json>").

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#include "motion.h"

struct vela_output;
struct vela_server;
struct wl_event_source;
struct wlr_keyboard;

struct vela_a11y {
    struct vela_server *server;

    bool night_light; // on, by hand or by the schedule
    int night_strength; // 0-100, like Windows' "Strength"
    bool night_nvidia_fix; // opt-in Vulkan rendering, bypass hardware gamma
    char schedule[16]; // "no", "sunset" (from sunset to sunrise), "hours"
    int night_from; // minutes after midnight
    int night_to;
    char schedule_key[64]; // the schedule read last time
    int scheduled; // what the schedule said last time (-1: unknown)
    // The gradual change between off (0) and on (1).
    double night_level;
    double level_from;
    struct vela_tween level_tween;
    bool level_animating;
    bool loaded; // vela.conf read once: what it says at startup is shown at once

    bool color_filter;
    char color_filter_kind[32]; // grayscale, deuteranopia, protanopia, tritanopia
    bool color_filter_shortcut; // Win+Ctrl+C

    bool magnifier;
    int zoom_step; // percent per Win+plus
    double zoom_target;
    double zoom; // the shown one (animated)
    double zoom_from;
    struct vela_tween zoom_tween;
    bool zoom_animating;
    struct vela_output *zoom_output; // the zoomed output (the cursor's)
    double view_x; // the zoomed area's corner, in the layout
    double view_y;
    // Where the cursor is drawn on the zoomed output, from its corner. While
    // the zoom animates the view is derived from it, never from the previous
    // frame's view, so where it ends depends on the zoom alone.
    double anchor_x;
    double anchor_y;

    bool sticky_keys;
    uint32_t latched; // modifiers pressed and released: they apply to the next key
    uint32_t locked; // pressed twice: they stay until pressed again
    uint32_t candidate; // the modifier pressed now, until something else comes

    struct wl_event_source *timer; // the schedule, every minute
};

struct vela_a11y *vela_a11y_create(struct vela_server *server);
void vela_a11y_destroy(struct vela_a11y *a11y);

// Rereads vela.conf (at startup and on "reload-config").
void vela_a11y_load(struct vela_a11y *a11y);

// save: also in vela.conf (not when the schedule decides).
void vela_a11y_set_night_light(struct vela_a11y *a11y, bool on, bool save);
void vela_a11y_set_color_filter(struct vela_a11y *a11y, bool on, bool save);
void vela_a11y_set_sticky_keys(struct vela_a11y *a11y, bool on, bool save);
void vela_a11y_set_magnifier(struct vela_a11y *a11y, bool on);
void vela_a11y_zoom(struct vela_a11y *a11y, int direction); // +1 Win+plus, -1 Win+minus

// The zoomed area follows the cursor (after each movement).
void vela_a11y_update_magnifier(struct vela_a11y *a11y);
// Animations (night light fading, magnifier opening) at `now_ms` (the frame's
// time), ended at `wall_ms`: false when nothing is left to animate.
bool vela_a11y_tick(struct vela_a11y *a11y, double now_ms, double wall_ms);
void vela_a11y_output_destroyed(struct vela_a11y *a11y, struct vela_output *output);

// Sticky keys: every key goes through here before the shortcuts. true if Win
// has just been latched for the next key (it doesn't open Start).
bool vela_a11y_sticky_key(struct vela_a11y *a11y, struct wlr_keyboard *keyboard, const xkb_keysym_t *syms,
    int count, bool pressed);

// The state for the shell: {"nightLight":true,...}. false if it doesn't fit in
// `size`.
bool vela_a11y_json(const struct vela_a11y *a11y, char *out, size_t size);

#endif
