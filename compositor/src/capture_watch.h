// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_CAPTURE_WATCH_H
#define VELA_CAPTURE_WATCH_H

// Tells the shell when the screen is being shared or recorded (the taskbar's
// indicator): "capture on" while a capture is streaming, "capture off" when it
// ends. Two sources count, both sustained, so one-off screenshots (Snipping
// Tool, grim) and Alt+Tab previews don't light it:
//  - ext-image-copy-capture sessions (the portal, OBS, Discord, the shell's
//    own tools) that stay open for over a second;
//  - wlr-screencopy frames found pending on consecutive checks (wf-recorder,
//    gpu-screen-recorder).
// "capture-indicator=no" in vela.conf turns it off.

struct vela_server;
struct wlr_ext_image_copy_capture_manager_v1;

struct vela_capture_watch *vela_capture_watch_create(struct vela_server *server);
void vela_capture_watch_destroy(struct vela_capture_watch *watch);
void vela_capture_watch_copy_capture(struct vela_capture_watch *watch, struct wlr_ext_image_copy_capture_manager_v1 *manager);
// vela.conf was read again.
void vela_capture_watch_load(struct vela_capture_watch *watch);

#endif
