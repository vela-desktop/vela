// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SNAPSHOT_H
#define VELA_SNAPSHOT_H

// A window's look "frozen": copies of its buffers, still valid when the app
// changes them or quits. The close, minimize and maximize animations move and
// scale the snapshot, not the real window (which is made of nested surfaces
// and can't be scaled).

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct vela_node;
struct vela_server;
struct vela_tree;
struct vela_view;

struct vela_snapshot;


// ------------------------------------------------------------ animations --

enum vela_snapshot_kind {
    VELA_SNAPSHOT_CLOSE, // shrinks and fades
    VELA_SNAPSHOT_MINIMIZE, // flies to its taskbar button
    VELA_SNAPSHOT_RESTORE, // the same flight, reversed
    VELA_SNAPSHOT_MORPH, // morphs into the new frame (vela_snapshot_morph)
};

// Running animations live in vela_server.snapshot_animations.
void vela_snapshots_init(struct vela_server *server);
void vela_snapshots_finish(struct vela_server *server); // ends them all, at shutdown

// Starts an animation of the window (cancelling the previous one). false if it
// doesn't start (window without a size).
bool vela_snapshot_animate(struct vela_server *server, struct vela_view *view, enum vela_snapshot_kind kind);
// Maximize and restore: the current content morphs into `to` (global frame)
// and fades while the real window appears there. Its corners go from the
// window's to `to_radius`, the ones it will have there.
bool vela_snapshot_morph(struct vela_server *server, struct vela_view *view, struct wlr_box to, double to_radius);
void vela_snapshot_cancel(struct vela_server *server, struct vela_view *view);
// The animations at `now_ms` (the frame's time), destroyed at `wall_ms`; true
// while some remain.
bool vela_snapshots_tick(struct vela_server *server, double now_ms, double wall_ms);

#endif
