// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SCENE_SURFACE_H
#define VELA_SCENE_SURFACE_H

// Vela's state tied to each wlr_surface: which outputs show it, and how much.
// From it come enter/leave, the preferred scale and the output that paces the
// surface's frame callbacks (docs/renderer.md §4.5, §3.6). Born with the
// surface (vela_scene_watch), dies with it (wlr_addon).

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/addon.h>

struct vela_scene;
struct wlr_output;
struct wlr_surface;

struct vela_surface_on_output {
    struct wlr_output *output;
    int64_t overlap; // surface pixels that fall on the output
    int64_t visible; // of those, how many aren't covered
};

struct vela_surface_state {
    struct wlr_surface *surface;
    struct vela_scene *scene;
    struct vela_surface_on_output *outputs;
    int output_count, output_capacity;
    // The output that shows the largest visible part: frame callbacks and
    // presentation come from it. NULL: the surface is covered or hidden and
    // the app may stop drawing.
    struct wlr_output *pacing;
    // Surface coordinates per logical unit: 1, except for X11 windows, whose
    // coordinates are pixels of their output (xwayland.c, §3.9).
    double density;

    struct wlr_addon addon;
    struct wl_listener commit;
};

// Creates the state of a new surface (from the scene).
void vela_surface_state_create(struct vela_scene *scene, struct wlr_surface *surface);
struct vela_surface_state *vela_surface_state_get(struct wlr_surface *surface);

// On every frame drawn: the surface covers `overlap` pixels of the output,
// `visible` of them uncovered.
void vela_surface_report(struct wlr_surface *surface, struct wlr_output *output, int64_t overlap, int64_t visible);
// It's no longer on that output.
void vela_surface_forget(struct wlr_surface *surface, struct wlr_output *output);

// The density of the surface tree `surface` belongs to (its root's): logical
// units are surface coordinates divided by it.
double vela_surface_density(struct wlr_surface *surface);
// Returns whether it changed.
bool vela_surface_set_density(struct wlr_surface *surface, double density);

#endif
