// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VELA_X11_SATELLITE_H
#define VELA_X11_SATELLITE_H

#include <stdbool.h>

struct vela_server;

// Start an optional native-resolution, session-wide X11 backend.
// The sockets are bound by Vela *before* launching the satellite process:
// clients can connect immediately without a startup race. Failure falls back
// to wlroots' existing integrated XWayland.
bool vela_x11_satellite_start(struct vela_server *server);
void vela_x11_satellite_stop(struct vela_server *server);
#endif
