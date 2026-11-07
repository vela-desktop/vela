// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_X11_ROOT_H
#define VELA_X11_ROOT_H

// The outputs as Xwayland sees them (docs/renderer.md §3.9): as large as their
// pixels, placed by x11_space.h. Xwayland gets an xdg-output of its own that
// says so, hiding wlroots' one (logical sizes, for every other client); RandR
// then offers games the native resolution. Follows the layout.

#include "x11_space.h"

#include <stdbool.h>

struct vela_server;
struct wlr_output;

struct vela_x11_root;

struct vela_x11_root *vela_x11_root_create(struct vela_server *server);
// After Xwayland's client is gone.
void vela_x11_root_destroy(struct vela_x11_root *root);

// An output in the root, NULL if it isn't in the layout.
const struct vela_x11_output *vela_x11_root_output(const struct vela_x11_root *root, const struct wlr_output *output);
// The output holding a root point, or the nearest; NULL without outputs.
const struct vela_x11_output *vela_x11_root_output_at(const struct vela_x11_root *root, double x, double y);
// Xft.dpi for X11 apps (vela_x11_dpi).
int vela_x11_root_dpi(const struct vela_x11_root *root);

#endif
