// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_WORKSPACE_H
#define VELA_WORKSPACE_H

// Virtual desktops, like Windows 11.
//
// Each window is on one desktop (or all: "Show this window on all desktops");
// only those on the current desktop are visible, the other trees are off. The
// taskbar shows only the current desktop's windows (their wlr-foreign-toplevel
// handle exists only for them), while the ext-foreign-toplevel list has them
// all: the shell's Task View shows their previews and knows where each is from
// the "workspaces" message (JSON) the compositor sends it on every change.
//
// The switch: the windows being left go to layers.windows_out, which slides
// away fading; the new ones (layers.windows) come from the other side. At the
// end the outgoing ones go back to layers.windows, off, in their order.
//
// Desktops and their names are remembered in ~/.config/vela/desktop.conf.

#include "motion.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vela_buffer;
struct vela_server;
struct vela_view;

struct vela_workspaces {
    struct vela_server *server;
    char **names; // one per desktop; "": "Desktop N"
    int count;
    int capacity;
    int current;
    char **sticky_apps; // their windows are on all desktops
    int sticky_count;
    int sticky_capacity;
    uint64_t next_map_serial;
    // The switch in progress: the outgoing windows (bottom to top) and its
    // direction.
    struct vela_view **outgoing;
    int outgoing_count;
    int outgoing_capacity;
    struct vela_tween tween; // linear: the slide applies the curve, the fade in doesn't
    int direction; // +1: moving right (the new desktop comes in from the right); 0: still
};

// Reads desktop.conf (there is always at least one desktop).
struct vela_workspaces *vela_workspaces_create(struct vela_server *server);
void vela_workspaces_destroy(struct vela_workspaces *workspaces);


// refocus_after: the keyboard goes to a window of the new desktop (not when
// it's already being given to a window there).
void vela_workspaces_switch(struct vela_server *server, int index, bool refocus_after);
int vela_workspaces_add(struct vela_server *server); // the new desktop's index
void vela_workspaces_remove(struct vela_server *server, int index);
void vela_workspaces_rename(struct vela_server *server, int index, const char *name);
void vela_workspaces_move(struct vela_server *server, int from, int to);
void vela_workspaces_move_view(struct vela_server *server, struct vela_view *view, int index);
void vela_workspaces_set_sticky(struct vela_server *server, struct vela_view *view, bool on);
void vela_workspaces_set_app_sticky(struct vela_server *server, const char *app_id, bool on);

// A new window: on the current desktop (or all, if its app is there).
void vela_workspaces_view_mapped(struct vela_server *server, struct vela_view *view);
void vela_workspaces_forget(struct vela_server *server, struct vela_view *view);
bool vela_view_on_current_workspace(const struct vela_view *view);

// The animated switch at `now_ms` (the frame's time); the outgoing windows go
// back to their layer at `wall_ms`. false once it's over.
bool vela_workspaces_tick(struct vela_server *server, double now_ms, double wall_ms);

// The state for the shell: {"current":0,"names":[...],"windows":{...},...}.
void vela_workspaces_json(struct vela_server *server, struct vela_buffer *out);
void vela_workspaces_announce(struct vela_server *server); // to the shell, on every change

#endif
