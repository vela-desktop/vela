// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_FOCUS_H
#define VELA_FOCUS_H

// The keyboard: which window or shell piece gets it. Like Windows, the focused
// window rises to the top (of the scene and of the Alt+Tab list), lights up
// for the app and for the taskbar, and gives the keyboard away only to a panel
// that asks for it (Start menu, quick settings). System dialogs waiting for an
// answer stay above normal windows.
//
// The state is in vela_server: focused_layer and previous_layer.

#include <stdbool.h>

struct vela_layer_surface;
struct vela_server;
struct vela_view;

void vela_focus_view(struct vela_server *server, struct vela_view *view);
// Reconcile each monitor's foreground fullscreen with windows and shell panels.
void vela_fullscreen_restack(struct vela_server *server);
// A window that appears or asks to come forward on its own (not by a click):
// if a system dialog is waiting, it stays below it and doesn't take its
// keyboard.
void vela_focus_new_view(struct vela_server *server, struct vela_view *view);
void vela_focus_layer(struct vela_server *server, struct vela_layer_surface *layer);
// The keyboard goes back to whoever should have it: the modal system dialog,
// the previous panel, or the most recent window on the current desktop (after
// unlocking, a close...).
void vela_focus_refocus(struct vela_server *server);
// The modal system dialog (polkit, docs/polkit-agent.md §7): a mapped
// "overlay" surface with exclusive keyboard interactivity, the most recent
// one. While it is there, keyboard and shortcuts are its own. NULL: none.
struct vela_layer_surface *vela_focus_modal_layer(struct vela_server *server);

// The window is gone (already out of the list): whoever remembered it (drag,
// Alt+Tab, clicks, snap layouts) forgets it; if it had the keyboard, the
// keyboard moves on.
void vela_focus_forget_view(struct vela_server *server, struct vela_view *view, bool was_focused);
void vela_focus_layer_unmapped(struct vela_server *server, struct vela_layer_surface *layer);
void vela_focus_forget_layer(struct vela_server *server, struct vela_layer_surface *layer);

#endif
