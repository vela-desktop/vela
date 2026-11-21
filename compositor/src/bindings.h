// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BINDINGS_H
#define VELA_BINDINGS_H

// Vela's shortcuts (like Windows: Win+L, Alt+Tab, Win+arrows, Win+D,
// Win+Ctrl+←/→...; inside another session, where the host keeps Super, the Alt
// variants), the window menu and its actions.

#include <stdbool.h>
#include <stdint.h>

#include "shortcuts.h"

struct vela_server;
struct vela_view;

// A pressed key (common/shortcuts/shortcuts.h), matched against Vela's
// shortcut table: true if it was a shortcut (and must not reach the app).
// *repeats: it repeats while held (volume up and down).
bool vela_bindings_handle(struct vela_server *server, const struct vela_press *press, bool *repeats);

// The window menu (docs/renderer.md §14.8), drawn by the shell at (lx, ly) in
// the layout. keyboard: opened from the keyboard (Alt+Space).
void vela_window_menu_show(struct vela_server *server, struct vela_view *view, double lx, double ly, bool keyboard);
// An action from the window menu, Task View, snap layouts or the taskbar:
// restore, move, resize, minimize, maximize, close, activate, snap-left,
// snap-right, "snap x0 y0 x1 y1 [quiet [window]]", activate-group, "move-to
// N", move-to-new, sticky, unsticky, app-sticky, app-unsticky.
void vela_window_action(struct vela_server *server, struct vela_view *view, const char *action);

#endif
