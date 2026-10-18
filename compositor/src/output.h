// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_OUTPUT_H
#define VELA_OUTPUT_H

// An output: where it is in the layout, the area left free by panels, its
// frame cycle (docs/renderer.md §4.3), the virtual vblank of headless outputs,
// VRR, power.
//
// Born when the backend announces a wlr_output (vela_output_create), dies with
// it: the wlroots destroy frees everything, the scene frame included.

#include "frame_clock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct vela_brightness_output;
struct vela_nested;
struct vela_output_frame;
struct vela_server;
struct wlr_output;
struct wlr_output_mode;
struct wlr_output_state;

// A logical rectangle whose edges fall exactly on an output's physical pixels
// (docs/renderer.md §3.5): the position can be fractional.
struct vela_area {
    double x, y, width, height;
};

// Where to put a window so it covers an area: exact logical position and
// integer size (what the client gets in the configure).
struct vela_placement {
    double x, y;
    int width, height;
};

#define VELA_OUTPUT_DELIVERIES 16

// A delivered frame, waiting for its cost to be measured.
struct vela_delivery {
    uint32_t seq;
    int64_t start; // when drawing should have started
    int64_t woke_at; // when it really started
    int64_t committed_at;
    uint64_t point; // GPU work; 0: none (scanout, cursor only)
    int timing_slot;
};

struct vela_output {
    struct wl_list link; // vela_server.outputs
    struct vela_server *server;
    struct wlr_output *wlr;
    struct wlr_box usable; // area free of panels (global coordinates)
    bool powered;
    // Where it was in the layout before being turned off (Settings,
    // wlr-randr): it's still announced, so whoever turns it back on puts it
    // there and not over another output at (0, 0).
    bool has_last_position;
    int last_x, last_y;
    // Only when Vela runs in a window inside another session.
    struct vela_nested *nested;
    // Its brightness (brightness.h); NULL: none to drive.
    struct vela_brightness_output *brightness;
    // From the scene to this output's pixels (scene/frame.h).
    struct vela_output_frame *frame;
    struct vela_frame_clock clock;

    // The rest is the frame cycle, private to output.c.
    struct wlr_output_mode *mode_before_off;
    int custom_before_off[3]; // width, height, mHz (outputs without modes)
    bool latching; // VELA_LATCH=0: draw right away, as before S3
    bool frame_requested; // something changed
    bool animation_requested; // the animations need their next vblank
    struct wl_event_source *idle_frame; // "frame" right away, if the output was idle
    int latch_fd;
    struct wl_event_source *latch_source;
    bool latch_armed;
    struct vela_frame_plan plan;
    bool planned;
    // The vblank an empty frame (nothing changed here) stood for: what the
    // animations show at that moment is on screen already, their next frame is
    // planned from it. 0: the last frame was drawn.
    int64_t empty_until;
    // In on_frame: a frame asked for meanwhile (the animations ask every
    // output, this one too) is planned after the drawing, by its vblank.
    bool drawing;
    struct vela_delivery deliveries[VELA_OUTPUT_DELIVERIES];
    int delivery_count;
    // What the cost is made of (VELA_STATS): late wakeup, CPU up to the
    // commit, wait before the GPU starts, GPU work.
    double breakdown_sum[4];
    double breakdown_max[4];
    int breakdown_count;
    bool vrr_unsupported;
    // Headless output: a virtual vblank exact to the nanosecond.
    int vblank_fd;
    struct wl_event_source *vblank_source;
    int64_t last_vblank;
    bool vblank_armed;
    bool awaiting_present; // a frame was delivered and waits for the vblank
    struct vela_delivery awaiting;

    struct wl_listener frame_event;
    struct wl_listener request_state;
    struct wl_listener present;
    struct wl_listener destroy;
};

// A new wlr_output from the backend: configures it (mode, scale, position
// saved or chosen by Vela) and puts it in the layout.
void vela_output_create(struct vela_server *server, struct wlr_output *wlr);

// Position and size in the global layout.
struct wlr_box vela_output_box(const struct vela_output *output);

// Where the compositor places windows, computed from physical pixels (§3.5):
// the whole output, the part free of panels, and an area in output pixels
// (relative to its corner) seen in logical units.
struct vela_area vela_output_full_area(const struct vela_output *output);
struct vela_area vela_output_usable_area(const struct vela_output *output);
struct wlr_box vela_output_physical_usable(const struct vela_output *output);
struct vela_area vela_output_from_physical(const struct vela_output *output, struct wlr_box physical);
// Position and integer size whose buffer, at this scale, covers exactly
// `area`. When exact pixels can't be had (at 150% 2560 pixels would be 1706.67
// units), the extra pixel ends up off the output when the area touches one of
// its edges; otherwise it stays one pixel inside, overlapping nothing.
struct vela_placement vela_output_place(const struct vela_output *output, struct vela_area area);

// Places panels and wallpapers (wlr-layer-shell) and recomputes the usable
// area; if it changes, maximized and snapped windows are rearranged.
void vela_output_arrange_layers(struct vela_output *output);

// Asks for a frame: at the next vblank the output redraws what changed (and
// frame callbacks go out).
void vela_output_schedule_frame(struct vela_output *output);

// The vela.conf choices that apply to all outputs: "variable-refresh" (no,
// games, always; VELA_VRR=0/1 wins) and "tearing" (VELA_TEARING=0 turns it
// off). Then a frame, to apply them.
void vela_output_load_settings(struct vela_server *server);

// Applies a mode or scale change, with the first frame already drawn at the
// new size.
bool vela_output_commit_mode(struct vela_output *output, struct wlr_output_state *state);

// Turns the output off and on (inactivity) while it stays in the layout: the
// shell doesn't lose its panels.
void vela_output_set_powered(struct vela_output *output, bool on);

// The output at a point of the layout, or NULL.
struct vela_output *vela_output_at(const struct vela_server *server, double lx, double ly);
struct vela_output *vela_output_named(const struct vela_server *server, const char *name);
// A window frame (global) put back into the output's usable area: first the
// size, then the position.
struct wlr_box vela_output_fit(const struct vela_output *output, struct wlr_box frame);

// The output under the cursor, or the first; NULL if there is none.
struct vela_output *vela_output_under_cursor(const struct vela_server *server);

// Make, model and serial number (the connector name when missing): the
// monitor's key in outputs.conf.
void vela_output_key(const struct wlr_output *output, char *out, size_t size);

#endif
