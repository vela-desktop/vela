// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_ACTIVATION_H
#define VELA_ACTIVATION_H

struct vela_server;
struct vela_view;

// App-initiated activation is separate from explicit taskbar/click/Alt+Tab
// focus. Wayland needs a token issued to the foreground surface; legacy X11
// requests cannot move a background app into the foreground.
void vela_activation_init(struct vela_server *server);
void vela_activation_finish(struct vela_server *server);
void vela_activation_request_x11(struct vela_view *view);

#endif
