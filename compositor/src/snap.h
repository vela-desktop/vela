// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SNAP_H
#define VELA_SNAP_H

// Window snapping, like Windows 11. A snapped window covers a rectangle of the
// usable area in twelfths: halves and quarters by dragging against edges and
// corners (the top maximizes it) and with Win+arrows; halves, thirds and
// quarters from the snap layouts the shell shows under the Maximize button
// (Win+Z).

#include <stdbool.h>
#include <stdint.h>

#include "motion.h"

struct vela_area;
struct vela_output;
struct vela_view;

// A rectangle of the usable area in twelfths; all zero: not snapped.
struct vela_snap {
    int x0;
    int y0;
    int x1;
    int y1;
};

static const struct vela_snap vela_snap_none = { 0, 0, 0, 0 };
static const struct vela_snap vela_snap_left = { 0, 0, 6, 12 };
static const struct vela_snap vela_snap_right = { 6, 0, 12, 12 };
static const struct vela_snap vela_snap_top_left = { 0, 0, 6, 6 };
static const struct vela_snap vela_snap_top_right = { 6, 0, 12, 6 };
static const struct vela_snap vela_snap_bottom_left = { 0, 6, 6, 12 };
static const struct vela_snap vela_snap_bottom_right = { 6, 6, 12, 12 };

static inline bool vela_snap_equal(struct vela_snap a, struct vela_snap b)
{
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

static inline bool vela_snap_is_none(struct vela_snap s)
{
    return vela_snap_equal(s, vela_snap_none);
}

static inline bool vela_snap_valid(struct vela_snap s)
{
    return s.x0 >= 0 && s.y0 >= 0 && s.x1 <= 12 && s.y1 <= 12 && s.x1 > s.x0 && s.y1 > s.y0;
}

// A snap's area on the output. The edges are taken in physical pixels:
// neighboring zones touch with no gaps or overlaps at any scale.
struct vela_area vela_snap_area(const struct vela_output *output, struct vela_snap snap);

// Snaps (or unsnaps, with vela_snap_none) the window; `output`: the output, if
// not the one it's on. animate: false when it leaves its snap because it's
// being dragged (it already follows the cursor).
void vela_view_set_snap(struct vela_view *view, struct vela_snap side, struct vela_output *output, bool animate);
// Realigns it to its snap on `output`.
void vela_view_apply_snap(struct vela_view *view, struct vela_output *output);
// Moved elsewhere: it's no longer with its group (a group of one window is no
// longer a group).
void vela_view_leave_snap_group(struct vela_view *view);

// ------------------------------------------------ dragging and the shell --
// While a window is dragged against an edge, a preview shows where it will end
// up; after a snap, the shell's Snap Assist offers the other windows for the
// spaces left free; windows arranged together form a group the taskbar brings
// forward together. Snap layouts open with Win+Z or with the mouse resting on
// the Maximize button.

struct vela_rect_node;
struct vela_server;
struct wl_event_source;

enum vela_snap_zone {
    VELA_SNAP_ZONE_NONE,
    VELA_SNAP_ZONE_TILE, // with `tile`
    VELA_SNAP_ZONE_MAXIMIZE,
};

struct vela_snapping {
    // The preview while dragging: where the window will snap if released now.
    enum vela_snap_zone zone;
    struct vela_snap tile;
    struct vela_output *output;
    struct vela_rect_node *rect; // below the dragged window, or NULL
    double target_x, target_y, target_width, target_height; // the target area
    struct vela_tween tween;
    // Snap layouts with the mouse resting on Maximize.
    struct vela_view *layouts_hover;
    struct wl_event_source *layouts_timer;
    uint32_t next_group;
};

struct vela_snapping *vela_snapping_create(void);
void vela_snapping_destroy(struct vela_snapping *snapping);

// Win+arrows, like Windows 11.
void vela_snap_keyboard(struct vela_server *server, struct vela_view *view, uint32_t sym);
// While dragging: the preview according to where the cursor is.
void vela_snap_update_zone(struct vela_server *server);
// The preview lighting up at `now_ms` (true while it animates). Nothing to
// retire: it ends with the drag.
bool vela_snap_tick_preview(struct vela_server *server, double now_ms);
// End of the drag; apply: the window snaps where it points.
void vela_snap_end_zone(struct vela_server *server, bool apply);
// After a snap: Snap Assist offers the other windows in the free spaces.
void vela_snap_offer_assist(struct vela_server *server, struct vela_view *view);
// Snap Assist put `view` next to `origin`: same group.
void vela_snap_join_group(struct vela_server *server, struct vela_view *view, struct vela_view *origin);
// The click on the group in the taskbar: all forward, `view` focused.
void vela_snap_activate_group(struct vela_server *server, struct vela_view *view);
// The window's snap layouts (keyboard: Win+Z).
void vela_snap_show_layouts(struct vela_server *server, struct vela_view *view, bool keyboard);
// The mouse on `view`'s Maximize (NULL: elsewhere).
void vela_snap_hover_maximize(struct vela_server *server, struct vela_view *view);
void vela_snap_forget(struct vela_server *server, struct vela_view *view);

#endif
