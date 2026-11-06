// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_CAPTURE_WATCH_H
#define VELA_CAPTURE_WATCH_H

// Who is capturing the screen or a window (docs/screen-sharing.md §3, §4):
// the clients other than the shell that keep an ext-image-copy-capture
// session open for 1 s, or ask for wlr-screencopy frames of one output again
// and again. "watch capture" on the command socket follows it, one JSON line
// with the whole state after every change; screenshots never show up.
// VELA_CAPTURE_UNREPORTED=1 in the compositor's environment reports nothing
// (recordings of Vela itself, §4.3).

struct vela_buffer;
struct vela_server;
struct wlr_ext_image_copy_capture_manager_v1;
struct wlr_screencopy_manager_v1;

struct vela_capture_watch *vela_capture_watch_create(struct vela_server *server,
    struct wlr_ext_image_copy_capture_manager_v1 *copy_capture, struct wlr_screencopy_manager_v1 *screencopy);
// After the clients are gone: wlroots wants no listener left on its globals.
void vela_capture_watch_destroy(struct vela_capture_watch *watch);

// Something a report names changed: a window's title or app id, an output.
void vela_capture_watch_refresh(struct vela_capture_watch *watch);

// The topic's line: {"clients":[...]} (§4.1).
void vela_capture_json(struct vela_server *server, struct vela_buffer *json);

#endif
