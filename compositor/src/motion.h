// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_MOTION_H
#define VELA_MOTION_H

// Vela's motion design: curves and durations in one place, so the whole
// desktop moves the same way.
//
// Four durations and one curve, the same as QML's (Theme.scale and
// Theme.decelerate in common/controls/Theme.qml). Both copies are read by
// scripts/check-shared-values.py, which fails if they disagree
// (docs/visual-coherence.md §6.4). Every animation below takes one of them.

#include <stdbool.h>

// A cubic Bézier curve like CSS/QML ones: cubic-bezier(x1, y1, x2, y2).
struct vela_curve {
    double x1, y1, x2, y2;
};

// x = normalized time [0, 1] -> progress [0, 1].
double vela_curve_eval(const struct vela_curve *curve, double x);

// A fixed-duration interpolation. Its start time is fixed by the first frame
// that reads it, so the first frame drawn is always exactly the initial state,
// however long passes between the request and the frame. One of duration 0 is
// finished on its first frame.
//
// Two clocks (docs/visual-coherence.md §6.2). Each output's frame shows the
// state at its own predicted presentation time (vela_tween_progress), which
// may go back from one output to the next. What can't be undone (destroying a
// snapshot, giving windows back to their layer) waits for the wall clock
// (vela_tween_retired): every frame still to come, on any output, is presented
// after it, so from then on all of them would show the final state anyway.
struct vela_tween {
    double start_ms; // < 0: not read yet
    double duration_ms;
    const struct vela_curve *curve; // NULL: linear
    const char *name; // for VELA_DEBUG_ANIMATION
    unsigned serial;
    bool retired;
};

void vela_tween_start(struct vela_tween *tween, const char *name, double duration_ms, const struct vela_curve *curve);
// The state at `now_ms`, a frame's presentation time: idempotent.
double vela_tween_progress(struct vela_tween *tween, double now_ms);
bool vela_tween_finished(const struct vela_tween *tween, double now_ms);
// Whether the wall clock (vela_now_ns(), in ms) has passed the end: the moment
// for what can't be undone.
bool vela_tween_retired(struct vela_tween *tween, double wall_ms);

// VELA_DEBUG_ANIMATION=1: every tween's progress on every frame, and a line
// when it retires, go to the compositor's log (the tests read it); NULL:
// nowhere.
extern void (*vela_motion_logger)(const char *line);

// Animation effects (animation-effects in vela.conf, Settings > Accessibility
// > Visual effects; docs/visual-coherence.md §6.5). Off, nothing moves: no
// rise, slide, scale, flight or zoom; what fades still fades, within
// VELA_MOTION_FAST_MS. Every animation asks vela_motion_effects() before using
// an amount of movement and passes its duration through vela_motion_ms().
void vela_motion_set_effects(bool on);
bool vela_motion_effects(void);
// How long animations take compared with Vela's durations
// (animation-duration in vela.conf, a percentage; Settings' slider): 1 is
// Vela's. With effects off, fades still take at most VELA_MOTION_FAST_MS.
void vela_motion_set_duration(double factor);
double vela_motion_ms(double duration_ms);

// The duration scale. fast: changes of state, small things (the snap
// preview, a magnifier step); normal: exits; slow: entrances; long: what moves
// across the whole output.
#define VELA_MOTION_FAST_MS 120.0
#define VELA_MOTION_NORMAL_MS 200.0
#define VELA_MOTION_SLOW_MS 250.0
#define VELA_MOTION_LONG_MS 300.0

// Deceleration, for entrances and exits alike: starts decisively and settles
// gently. This curve, more than the duration, is what feels "modern". An exit
// curve that starts slowly would make a window seem to ignore the click for
// half the time; exits are shorter instead. Beware of more extreme curves such
// as (0.1, 0.9, 0.2, 1): at 180 Hz they cover half the movement in the first 3
// frames and the animation is no longer visible.
#define VELA_DECELERATE_CURVE 0.0, 0.0, 0.2, 1.0
extern const struct vela_curve vela_decelerate;

// Window open: fade plus rise.
#define VELA_WINDOW_OPEN_MS VELA_MOTION_SLOW_MS
#define VELA_WINDOW_OPEN_RISE 36

// Close: the window shrinks a little and disappears.
#define VELA_WINDOW_CLOSE_MS VELA_MOTION_NORMAL_MS
#define VELA_WINDOW_CLOSE_SCALE 0.94

// Minimize and restore: the window flies to its taskbar button (and back),
// shrinking and fading.
#define VELA_WINDOW_MINIMIZE_MS VELA_MOTION_SLOW_MS
#define VELA_WINDOW_MINIMIZE_SCALE 0.3

// Maximize, snap (a half, a quarter, a layout's zone) and back: the old
// content morphs into the new frame and fades into the real window.
#define VELA_WINDOW_MAXIMIZE_MS VELA_MOTION_SLOW_MS

// Snap preview: grows from the center of the area and lights up.
#define VELA_SNAP_PREVIEW_MS VELA_MOTION_FAST_MS

// Desktop switch: the desktop being left slides away fading, the new one comes
// from the other side. The slide is a fraction of the narrowest output: with
// several outputs windows don't intrude on the next one.
#define VELA_WORKSPACE_SWITCH_MS VELA_MOTION_LONG_MS
#define VELA_WORKSPACE_SLIDE_FRACTION 0.25

// Magnifier: from one zoom level to the next.
#define VELA_MAGNIFIER_MS VELA_MOTION_FAST_MS

#endif
