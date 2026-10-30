// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

struct vela_server;
struct vela_x11_xdg_output;

// Experimental per-Xwayland-client xdg-output geometry adapter. This is a
// KWin-style protocol building block, NOT the final surface/input scale.
struct vela_x11_xdg_output *vela_x11_xdg_output_create(struct vela_server *server);
void vela_x11_xdg_output_destroy(struct vela_x11_xdg_output *manager);
