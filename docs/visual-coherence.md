# Visual coherence: effects, motion, decorations

How Vela becomes one desktop that looks and moves the same way everywhere:
the compositor and its renderer, the Qt shell, and Vela's apps. This document
comes before the code. It lists what is wrong today, what is different on
purpose, the invariants the code must keep, and the changes, in the order
they are made and with how each is verified.

> **Status:** approved design, implemented in the order of §8; done: PR 1
> (§6.1), PR 2 (§6.2), PR 3 (§6.3), PR 4 (§6.4), PR 5 (§6.5), PR 6 (§6.6),
> PR 7 (§6.7), PR 8 (the shared pieces of §6.8 and §6.9, §6.11 reporting),
> PR 9 (§6.10), PR 10 (§6.9 for the other panels), PR 11 (the rest of §6.8,
> §6.11 failing).
> Each change has one explanation, in §6; the plan in §8 only says which pull
> request carries it.

## 1. Goals

- **Motion comes with the control.** Whoever puts a button, a switch, a list
  row or a panel on screen gets Vela's hover, press, state and entrance
  transitions without writing them. Quick Settings visibly moves.
- **One look.** The same timings and curve in the compositor and in QML; the
  same colors where the compositor and Qt draw side by side (the title bar and
  the app's content); the same shadows on everything that floats.
- **Less motion on request.** One accessibility switch reaches every animation,
  in every process.
- **No regression** in sharpness at fractional scales, timing on monitors with
  different refresh rates, blur quality, latency or idle cost
  ([renderer](renderer.md)).

## 2. Invariants

Every change in §6 follows from these.

1. **Pixels.** Logical coordinates become physical pixels in one place
   (`scene/frame.c`), rounding edges once, and an image drawn at the output's
   scale is copied 1:1 *whoever drew it*. Two roundings, or a rule for apps
   only, make the same text sharp in one place and soft in another.
2. **Time.** A frame shows the animated state at *its own* predicted
   presentation time; anything that can't be undone (destroying a snapshot,
   giving windows back to their layer) happens only once the wall clock has
   passed the end, because outputs draw in wall-clock order, not in
   presentation order.
3. **Who owns a surface animates it and gives it its shadow.** The compositor
   for windows, the shell for its panels: each is the only one that knows the
   shape and the meaning of what moves.
4. **Mechanism for clients, policy for ourselves.** A client asking for blur gets
   blur; Vela's acrylic recipe is for Vela's surfaces.
5. **Qt behaves, Vela looks.** Input, keyboard, focus, states and accessibility
   come from Qt Quick Templates; Vela draws them and times their changes.
6. **States fade, values follow.** A change of state (hover, press, on/off,
   selected, open) is animated; a value that tracks input (a drag, a slider
   under the pointer, scrolling, a level meter) never is.
7. **Every duration is a token, every movement can be switched off.** That is
   what lets one accessibility switch reach everything.
8. **Few shared numbers, checked; computed colors, sent.** The handful of
   timings both processes use are written on both sides and kept equal by a
   test. Colors the compositor needs from the shell's theme arrive already
   computed, so each formula lives in one place.

## 3. Responsibilities

| Part | Owns | Never |
|---|---|---|
| Renderer (`compositor/src/render/`) | quads, rounded clip, analytic shadow, blur, dither, color filters, frame timing | knowing windows, themes or time |
| Scene (`compositor/src/scene/`) | flattening, the conversion to pixels and its snapping, occlusion, damage, window shadow depths, the blur protocol and its policy (§6.7) | deciding when or where something moves |
| Compositor policy (`view.c`, `snapshot.c`, `workspace.c`, `snap.c`, `a11y.c`, `decoration.c`) | window animations and their recipes (`motion.h`), shapes, the title bar, the snap preview, magnifier, night light | computing the theme's colors (it receives them, §6.6) |
| Shell (`shell/qml`, `shell/src`) | its panels: look, motion, shadow, blur region, window size; the theme's colors, sent to the compositor | moving windows |
| `Vela.Controls` (`common/controls`) | `Theme` (colors, radii, tokens) and the pieces used by more than one program: `Fill`, `Button`, `Switch`, `Slider`, `TextBox`, `Dialog`, `Shadow`, `PanelMotion` | controls used by one program only: they stay with it |
| Settings, Files, polkit prompt | their screens and their own controls, built on Templates and `Fill` | repeating a transition a shared piece already has |

## 4. Problems

What the code does today and why it is accidental. The fix is the change in
§6 named in brackets.

### Compositor

**D1. The compositor's own images are not snapped like app surfaces** [§6.1].
`scene/frame.c` `add_surface()` forces an element's box to the buffer's size
when `vela_buffer_matches_area()` says the app drew at the output's scale. The
`VELA_NODE_BUFFER` case of `visit()` doesn't: the title, the app icon and the
button glyphs (`decoration.c` `set_image()`) only get the edge rounding, so
their width is `lround((x + W/s)·s) − lround(x·s)`, which is `W` only by
floating-point luck. Window positions come from physical pixels
(`vela_place_axis()`, maximize, snap), so `x·s` often lands on .5, where the
luck runs out. Simulated over 3000 origins × 40 offsets × 28 widths: 1.7% of
boxes at 125%, 2.2% at 150%, 1.1% at 175%, 2.4% at 225% are one pixel off, and
`is_one_to_one()` sends the image through the bilinear filter. The rule was
written for surfaces (S2); the bar (S6) reused buffer nodes.

**D2. Two animations round positions to logical units** [§6.1]. `view.c`
`apply_open_frame()` (`round((1.0 - progress) * rise)`) and `workspace.c`
`vela_workspaces_tick()` (`round(-distance * t)`). At 125% a unit is 1.25
pixels: the motion advances 1 and 2 pixels alternately and, in the curve's slow
tail, stalls on a unit for several frames at 180 Hz. Rounding twice, against
invariant 1.

**D3. One animation clock for all outputs** [§6.2]. `server.c`
`vela_server_animate()` sets `animation_now_ms = fmax(animation_now_ms,
present)`. Each output passes its own predicted presentation time,
`deadline + latency_frames × period` (`frame_clock.c`
`vela_frame_clock_plan()`): its lead over the wall clock is the frame budget
plus any learned latency. The maximum makes an output draw another output's
future. On DRM with late latching the leads differ by about 1 ms; nested in KWin
(margin at maximum, one or two frames of learned latency) or with
`VELA_LATCH=0` the difference reaches a period or more. With 180 + 75 Hz the
180 Hz output shows the animation 13 ms or more ahead, then repeats the same
state until its own time catches up: a stutter on the fastest monitor. The
`fmax` is not arbitrary, though: some ticks do irreversible things (§6.2), and
without it they would run at the wrong time.

**D4. Five animation drivers, each slightly different** [§6.2].
`vela_server_animate()` names the running animations twice: in an early return
(`vela_views_animating()`, `vela_snapshots_running()`,
`vela_snap_preview_shown()`, `workspaces->direction`, `vela_a11y_animating()`)
and in the ticks; an animation forgotten in the first list never starts.
`workspace.c` re-implements `vela_tween` inline; `snap.c`
`vela_snap_tick_preview()` uses `now_ms == 0.0` for "before the first frame";
`a11y.c` `vela_a11y_load()` uses `animation_now_ms == 0.0` for "at startup".

**D5. A morphing window's corners grow instead of going away** [§6.3].
`snapshot.c` `snapshot_apply()` scales the radius with the snapshot
(`radius * fmin(scale_x, scale_y)`): maximizing a half-screen window, the
corners grow to 16 while the window it becomes has none.

### Compositor and shell

**D6. Third-party apps get Vela's acrylic** [§6.7]. `blur_mix.frag` applies to
every `ext-background-effect-v1` region a saturation of 1.25, the gray
`scene->acrylic_tint` (Windows' dark or light gray at 55%, chosen by the
*shell's* mode in `command.c` `theme()`) and a dither. The protocol says "the
blur algorithm is subject to compositor policies": saturation is a defensible
policy of a blur; a gray veil switched by Vela's theme is not blur, and on KWin
the same app gets a plain one.

**D7. The snap preview ignores the accent** [§6.6]. `snap.c` `preview_color` is
the default accent `#5b8cff`, but the accent is the user's choice
(`appearance/accent`, `shell/src/appearance.cpp`) and the compositor is never
told. It is also the only floating thing in Vela with square corners.

**D8. The Mica formula exists twice** [§6.6]. `decoration.c` `mica_color()` and
`shell/src/mica.h` `micaFromTint()`, kept equal by comments. If they drift, the
seam between the title bar (compositor) and the content of Settings or Files
(Qt) shows.

**D9. The title bar's buttons use their own gray and don't move** [§6.6]. `decoration.c` hovers with white 8% (dark) or black 6%
(light); WinUI's SubtleFillColorSecondary, which `Theme.qml` follows as
`subtleHover`, is 6.1% and 3.7%. Every other hover in Vela will fade (§6.8);
the bar's switches at once.

**D10. The motion numbers are shared only on paper** [§6.4]. `motion.h` says
the shell uses the same values "see shell/qml/Theme.qml" (the file is now
`common/controls/Theme.qml`). Only 250 is on Theme's scale (120, 200, 250):
window close is 180, snap preview 150, magnifier 150, desktop switch 300;
Settings' page transition (`settings/qml/Main.qml`) has a literal 300;
`polkit/prompt/main.cpp` hard-codes `200 + 60` as "Theme.normal plus a margin".
The exit curve contradicts itself: `motion.h` uses decelerate for close
because "with one that starts slowly the window would seem to ignore the
click", while the shell uses `Theme.accelerate`, a slow start, for Start's and
the polkit prompt's exits.

**D11. Motion can't be reduced** [§6.5]. Windows has Accessibility > Visual
effects > Animation effects; Vela's accessibility keys in `vela.conf` (color
filters, magnifier, sticky keys) have nothing like it. Every animation this plan
adds makes that worse unless the switch is designed in.

### Shell and apps

**D12. Controls re-implement Qt and change state instantly** [§6.8]. About 158
visual properties across shell, Settings, Files and the polkit prompt are
ternaries on `containsMouse`, `pressed`, `checked`, `selected`, against about 32
`Behavior`s: four state changes out of five jump. The controls are hand-made
from `Rectangle` and `MouseArea`, re-implementing what Qt Quick Templates
already does: `common/controls/Button.qml` writes its own `Keys` handlers and
`Accessible` properties; Quick Settings has private `Tile`, `Slider`, `Switch`
and `Link`; Settings `Toggle.qml` and `Slider.qml`; Properties a `Check`; the
night light page a `Radio`; the file dialogs a `Field`; the Notification Center
a `SmallButton`; Files a `ToolButton`. Each copy decides alone whether to
animate, and most don't. Yet `TextBox` (a `QtQuick.Controls.Basic` `TextField`)
and Settings' `Choice` (a `ComboBox`) already show the other way: Qt's
behavior, Vela's look.

**D13. Quick Settings doesn't move** [§6.10]. It enters with a fade and 12 units
of slide (barely visible) and leaves at once. A tile turning on jumps to the
accent; hover and press jump; the slider's thumb jumps; a Wi-Fi network's
password field jumps open; going back plays the same slide as going forward;
the height jumps between pages, because the window is resized with the panel
(`height: panel.height + 24`), which a layer surface can't do smoothly.

**D14. Shadows: four panels of seventeen, four recipes, cut by their
windows** [§6.9]. `PanelShadow` (blur 32, offset 6, 45%) on the file dialogs;
context menus (16, 4, 40%), taskbar previews (24, 4) and Start (40, 8, 50%)
override it; the polkit
prompt draws its own (48, 16, 45%) because `PanelShadow` lives in `Vela.Shell`.
Quick Settings, Notification Center, Clipboard, Alt+Tab, Snap Layouts, Run,
Confirm, Properties, the screen-share chooser, toasts, the OSDs and the taskbar
tooltip have none. Windows leave 10–12 units of room around the panel, less
than a shadow's reach (46), so even the existing shadows are cut.

**D15. Entrances written fifteen times, and drifting** [§6.9]. Opacity eased in
most and linear in Alt+Tab, Snap Layouts, Task View, the desktop OSD and the
in-app dialogs; dialogs scale from 1.05 (Settings, Files, polkit: WinUI's
ContentDialog), from 0.96 (`FileDialogs`) or not at all (Run, Confirm,
Properties, the screen-share chooser).

**D16. Only Start leaves with an animation, and it jumps when reopened**
[§6.9]. Quick Settings, Notification Center and Clipboard set
`visible = false`. Start animates out and survives a reopening during the
close (`test_shell.py`,
`test_reopening_during_close_animation_keeps_the_menu_open`), but its `open()`
resets `panel.opacity` and `panel.y` to the start: it jumps instead of turning
back.

**D17. Sliding panels leave their blur behind** [§6.9]. Start animates `panel.y`
and updates its blur region on every change. Quick Settings, Notification
Center, Clipboard, taskbar previews, the volume OSD and context menus slide
with a `Translate` and set the region at the final position: for the first
frames a band as tall as the slide has no blur behind it.

**D18. Two dialogs** [§6.9]. `settings/qml/Dialog.qml` and
`explorer/qml/Dialog.qml` are the same component with two APIs and drifting
values (Files uses literals where Settings uses `Theme`, and the other way
round for the footer).

### Documents

**D19. Documents that describe what the code doesn't do** [§8, with each PR].
renderer.md §5.1 gives nodes "a transform (translation + scale)": they have a
position and an opacity. §9.4 reads a tint per position from the wallpaper's
mipmaps: the code uses one average color from the shell. §8.3 doesn't say the
tint reaches third-party apps. `motion.h` points at a moved file.

## 5. Differences that stay

**K1. Two animation engines.** The compositor runs `vela_tween` at each
output's predicted presentation time, because it draws several outputs on their
own clocks. Qt runs its own engine, paced by the frame callbacks the compositor
sends each surface, with interruption and reversal solved and tested. A shared
engine would rewrite Qt's or teach Qt the compositor's clock; what must match
is the numbers and the curve (§6.4).

**K2. Window shadows in C, panel shadows in Qt.** Same model (the closed-form
Gaussian of a rounded rectangle, drawn only outside the shape so acrylic never
shows its own shadow), two owners (invariant 3). Different depths are WinUI's
elevations: windows (two layers, stronger when active), flyouts, dialogs.

**K3. The acrylic gray stays in the compositor, for Vela's panels.** It looks
like a split responsibility (gray in `blur_mix.frag`, fill in `Theme.qml`), and
moving it into the fill was this plan's previous version. It is wrong: the
blurred background is quantized when `blur_mix.frag` writes it, and the dither
added there only survives what is drawn after it. The panel's fill is blended
on top by fixed-function blending, where nothing can dither. Today the dither
reaches the screen at 0.38 of its amplitude (under a 62% fill); with the gray
moved into the fill it would reach it at 0.17 (under 83%), too weak, and
gradients behind the taskbar would band. Raising it would make third-party
blurs noisy. So the acrylic material is: gray, saturation and dither by the
compositor, fill by the shell, tuned together, for Vela's surfaces only (§6.7).

**K4. A solid Mica bar, blurred acrylic panels.** Windows' two materials: Mica
doesn't show the windows behind it.

**K5. Menus, tooltips, previews, Alt+Tab and Task View appear animated and
vanish at once.** renderer.md §14.3, as on Windows: the next action is
elsewhere.

**K6. Opacity per element, not per group.** A fading window is drawn element by
element, so overlapping parts show through each other (the outgoing desktop's
windows, as `vela_workspaces_tick()` notes). A group opacity needs an offscreen
image per animated window per frame; nobody has reported the artifact.

**K7. Corners only on free windows.** None when maximized, snapped, fullscreen,
or for apps with their own shadow margins (GTK): as on Windows.

**K8. Different slide distances.** Start rises 140 units from behind the
taskbar; Quick Settings and Clipboard 48; the volume OSD 12; menus 8; the
Notification Center 24 sideways. Each comes from its own edge, by its own
amount: a parameter of each panel, not a table.

**K9. Shapes recomputed every frame.** `vela_view_update_shape()` for every
window in `vela_server_animate()`: a few comparisons, and no invalidation bugs.

**K10. The blur weakens above 240%.** `blur_strength()` clamps at 3: four
Kawase levels show artifacts beyond. Written in renderer.md §8.3 with §6.7.

**K11. Shell mode and apps mode.** Panels follow the shell's mode, title bars
the apps' mode: the bar belongs to the app window.

**K12. Files' rows keep their own input.** `explorer/qml/DetailsView.qml` and
`IconView.qml` handle selection with modifiers, rubber-band selection, drag and
drop and renaming in their own `MouseArea`s; an `ItemDelegate` would fight
them. They only take `Fill` (§6.8).

**K13. The polkit prompt keeps its lifecycle.** Its exit is driven by the agent
(`Request.closing`) and the process waits for the fade (`fadeOutMs` in
`polkit/prompt/main.cpp`) before exiting. It takes the tokens, `Shadow` and
`Fill`, not `PanelMotion`.

**K14. Basic-style controls stay.** `TextBox` and `Choice` are restyled
`QtQuick.Controls.Basic` controls and work; rewriting them on Templates would
change nothing visible.

## 6. Changes

### 6.1 Snap every image; round once (D1, D2)

The rule `add_surface()` already has becomes a function, called for app
surfaces and for buffer nodes:

```c
// An image drawn at the output's scale covers exactly its own pixels
// (docs/renderer.md §3.4), whoever drew it: an app, or the compositor (title,
// icon, glyphs).
static void snap_to_buffer(struct wlr_box *box, const struct wlr_fbox *src, enum wl_output_transform transform,
    double width, double height, double scale)
{
    bool swapped = transform & WL_OUTPUT_TRANSFORM_90;
    double buffer_width = swapped ? src->height : src->width;
    double buffer_height = swapped ? src->width : src->height;
    if (vela_buffer_matches_area(src->x, src->y, buffer_width, buffer_height, width, height, scale)) {
        box->width = (int)buffer_width;
        box->height = (int)buffer_height;
    }
}
```

Snapshot pieces are unaffected while they scale (their buffers don't match the
area); at scale 1 they now snap like the live window, so the hand-over is
seamless. The open and desktop-switch animations pass exact logical positions:
the two `round()`s go.

**Verified by** a functional test: a decorated window placed at 40 positions at
125, 150, 175 and 225% must show a title whose pixels are identical in every
position (a 1:1 copy of the same raster is the only way they can be).
`scripts/test-sharpness.sh` stays bit for bit.

### 6.2 Each output samples, the wall clock retires (D3, D4)

Every frame is "tick, then flatten", synchronously, in `output.c` `on_frame()`:
`vela_server_animate(plan.present)` writes the animated state into the shared
scene, and `vela_output_frame_render()` reads it at once and diffs it against
*that output's* previous frame. So rewriting the scene for a different time on
each output is safe. What isn't safe is the irreversible part of the ticks:

| Tick | Writes every frame (can go back) | Does once (can't be undone) |
|---|---|---|
| `vela_views_tick` (`view.c`) | the window tree's position and opacity | at the end, `animating = false`: `vela_view_keep_in_place()` takes over |
| `vela_snapshots_tick` (`snapshot.c`) | the snapshot's pieces, opacity, shape; the real window's opacity (morph) | at the end, the snapshot is destroyed; a restore enables the window |
| `vela_workspaces_tick` (`workspace.c`) | the two window layers' position and opacity | at the end, `finish_switch()` gives the outgoing windows back to their layer |
| `vela_snap_tick_preview` (`snap.c`) | the rectangle | nothing (it ends with the drag) |
| `vela_a11y_tick` (`a11y.c`) | night light gains, magnifier zoom and area, the cursor on the zoomed output | at the end, the animating flags; a closing magnifier forgets its output |
| all, through `vela_tween_progress()` | | the start time, fixed by the first read |

Outputs draw in wall-clock order, not in presentation order. B (75 Hz, nested,
lead 13 ms) ticks at wall time 9 for presentation 22; A (180 Hz, lead 1 ms)
ticks at wall time 10 for presentation 11. If a close ends at 20 and B's tick
destroys the snapshot, A's frame at 11 has none: the close loses its last 9 ms
on A. The `fmax` prevents that by never letting time go back, at the price of
D3. The smallest rule that keeps both properties:

- **State at the frame's time.** A tick applies `vela_tween_progress(&t, now)`
  with `now` = the drawing output's predicted presentation time. Applying is
  idempotent and may go back in time from one output to the next.
- **Retirement on the wall clock.** The irreversible part runs on
  `vela_tween_retired(&t, wall)` with `wall` = `vela_now_ns()`; both are
  `CLOCK_MONOTONIC` (`util.c`). Any frame still to be drawn, on any output, is
  presented after `wall` (`vela_frame_clock_plan()` only picks vblanks after
  now), so once `wall` passes the end every remaining frame would show the final
  state anyway. Until then the tick keeps writing the final state.
- **Start on first read**, unchanged: an output with an earlier presentation
  time sees the initial state (progress is clamped), never a jump back.
- **Input ends things at once**, unchanged: `vela_snapshot_cancel()`, a new
  switch calling `finish_switch()`, a drag. Every output's next frame shows it.

```c
void vela_server_animate(struct vela_server *server, int64_t present_ns)
{
    vela_xwayland_sync(server);
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        vela_view_update_shape(view);
    }
    // What this frame shows (docs/renderer.md §4.2), and what can be retired:
    // no frame still to come is presented before the wall clock.
    double now_ms = present_ns / 1e6;
    double wall_ms = vela_now_ns() / 1e6;
    bool running = vela_a11y_tick(server->a11y, now_ms, wall_ms);
    running |= vela_views_tick(server, now_ms, wall_ms);
    running |= vela_snapshots_tick(server, now_ms, wall_ms);
    running |= vela_snap_tick_preview(server, now_ms);
    running |= vela_workspaces_tick(server, now_ms, wall_ms);
    if (running) {
        vela_server_schedule_frames(server);
    }
}
```

With it go `animation_now_ms`, the early return and its four queries (an idle
tick checks one flag), `workspace.c`'s inline tween (a `vela_tween`), `snap.c`'s
`0.0` (the preview's first state is applied directly, as
`start_open_animation()` does) and `a11y.c`'s startup test (a `loaded` flag).

Consequences, all bounded by the largest lead (frame budget plus learned
latency: about 1 ms on DRM with late latching, a few periods nested):

- frames keep being planned until the wall clock passes the end; they carry no
  damage, and `vela_output_frame_render()` commits nothing without damage. Such
  an empty frame stands for its vblank: the next one is planned from there.
  Found while implementing: before, an output with nothing to draw planned
  frame after frame at the event loop's speed while an animation ran elsewhere
  (without late latching, or before its first vblank: a hot-plugged output), and
  the frame the ticks ask of the output that is drawing drew the same vblank a
  second time. A request made while drawing is now planned by that frame's
  vblank;
- on an output with a shorter lead, an animation starts when the first output to
  read it says so: a small delay, never a jump;
- hit testing (`vela_scene_at()`) reads the positions last written: a click during
  an animation lands where one of the outputs shows the window, as today;
- with every output off nothing ticks, as today; the first frame after waking
  retires what was under way, the wall clock being long past, and starts what
  was asked for meanwhile (start on first read).

One diagnostic stays, in the family of `VELA_DEBUG_DAMAGE` and
`VELA_DEBUG_SCANOUT`: `VELA_DEBUG_ANIMATION=1` logs, per output and frame, the
presentation time, the wall time and each running animation's progress, and a
line when an animation retires. The tests read it.

**Verified by** a functional test with two headless outputs at 75 and 180 Hz,
with and without `VELA_LATCH=0`, running close, minimize, restore and a desktop
switch: on each output every logged progress equals the curve at that output's
own time (the time within 1e-6, the curve within 1e-5: it is solved to 1e-6)
and doesn't decrease (except across a mode change, which resets the grid);
both outputs draw every animation, at most one frame per vblank; no retirement
is logged before the wall clock passes the end; every started animation
retires; one second after the last one, the outputs' frame counters in the test
state stop.

### 6.3 Corners follow the morph (D5)

The morph's radius goes from the source's to the destination's, like position
and size: one more pair of values in `snapshot.c`'s `struct animation`.

**Verified** with the animation log: the source's radius on the first frame,
the destination's on the last.

Snapping (Win+arrows, dragging to an edge, a layout's zone) and unsnapping
morph the same way, as on Windows 11; leaving a snap or maximized by dragging
the window doesn't, it already follows the pointer. With two outputs every
taskbar reports the window's button, and minimizing flies to the one on the
window's own output. `test_animation.EveryTrigger` starts each window animation
the ways a person does (the title bar's buttons and double click, the
keyboard, dragging to an edge) and requires its snapshots to hold the app's
surfaces; the shell's tests do the same with Settings, a Qt app drawn on the
GPU, and its taskbar button.

### 6.4 One duration scale, written twice and checked (D10)

| Token | ms | Compositor (`compositor/src/motion.h`) | QML (`Theme.qml`) |
|---|---|---|---|
| fast | 120 | snap preview (was 150), magnifier (was 150), title bar fades | fades in, state fills |
| normal | 200 | window close (was 180) | exits, popups, dialogs, page changes, panel height |
| slow | 250 | window open, minimize, restore, maximize | flyouts entering |
| long | 300 | desktop switch | Settings' page transition (literal today) |

One curve, decelerate `(0, 0, 0.2, 1)`, for entrances and exits, exits shorter;
`Theme.accelerate` goes. Night light keeps its one second: Windows' value, and
it isn't motion.

The four numbers and the curve are written in `motion.h`
(`VELA_MOTION_FAST_MS` and the others, `VELA_DECELERATE_CURVE`; each animation's
duration is defined as one of them) and in `Theme.qml`
(`readonly property var scale: ({ fast: 120, normal: 200, slow: 250, long: 300 })`,
from which `Theme.fast` and the others derive). A ctest script,
`scripts/check-shared-values.py`, reads both, plus `fadeOutMs` in
`polkit/prompt/main.cpp` (which must be at least `normal`) and, from PR 6, the
title bar's fills (§6.6), and fails if they disagree. Why not one shared header: QML would
read four numbers through a C header, a C++ property and `Theme`, three hops
for a reader looking for `Theme.fast`, plus an include path from the
compositor into `common/`; a twenty-line test keeps two readable copies equal.

**Verified by** the script in ctest (`shared-values`, with its own unit tests
in `scripts/tests`), and the window animations' logged durations
(`test_animation.py`, `Durations`).

### 6.5 Animation effects: one switch (D11)

`animation-effects` in `vela.conf` (default `yes`), next to the other
accessibility keys, shown as Settings > Accessibility > Visual effects >
Animation effects, as on Windows. Off means:

- **no movement**: slides, rises, scales, the minimize flight, the desktop
  slide, the magnifier's zoom (it happens at once), layout changes (a panel's
  height, a field opening); maximize and restore don't morph, the window takes
  its new frame at once;
- **fades kept, at most `fast`**: a window that closes fades where it is, a
  minimized one fades out, a desktop switch cross-fades. A change stays visible
  as a change;
- **state fills unchanged** (`fast`); night light unchanged.

One switch rather than levels: fades are what keep a change understandable,
and Windows offers exactly this. The plumbing sits where durations and amounts
are already read:

- **Compositor.** `vela_a11y_load()` reads the key on `reload-config`, as it does
  for color filters, and stores it in `motion.c`. Window animations ask
  `vela_motion_effects()` before using an amount
  (`rise = vela_motion_effects() ? VELA_WINDOW_OPEN_RISE : 0`) and pass
  durations through `vela_motion_ms()`, which caps them at `fast` when effects
  are off. A `vela_tween` of duration 0 is finished on its first frame.
- **QML.** `Style` (`Appearance`) watches `vela.conf` as it watches
  `vela-shell.conf` and exposes `animationEffects`. `Theme` caps its tokens and
  exposes `Theme.motion` (1 or 0), which multiplies every movement:
  `PanelMotion`'s offset and scale, a page's slide, the duration of a height
  change.

Until `PanelMotion` exists (PR 8), the shell's panels, Settings' pages and
dialogs, and the polkit prompt multiply their existing offsets and scales by
`Theme.motion`, and their layout `Behavior`s are enabled only with it.

**Duration.** Below the switch, Settings has a slider for how long animations
take: `animation-duration` in `vela.conf`, a percentage of Vela's durations
from 50 to 300 in steps of 25, 100 by default. The slider marks Vela's 100
with a tick and the label "(suggested)", and a double click on its knob goes
back to it. The compositor multiplies every duration in `vela_motion_ms()`
(night light aside, it isn't motion); `Style` exposes it as
`animationDuration`, and `Theme`'s tokens are `scale` times it, so one factor
reaches both processes and the shared values stay the ones checked in §6.4. The
polkit prompt waits for its fade times the same factor. With effects off the
slider is disabled: fades take at most `fast`, whatever it says. Added at the
user's request: the person who wants to follow a window as it moves (or see
less of it) gets one knob, distinct from the switch; it isn't §9's rejected
levels, which reduced *what* moves.

**Verified by** the animation log for each window animation with the switch off
(each recipe logs its `geometry`, position and scale, which stays put; every
duration is `fast`, the magnifier's 0; no morph), a Qt Quick Test for
`PanelMotion` (§6.9), a functional test that flips the switch through
Settings' `vela.conf` and `reload-config` without restarting anything
(`test_animation.AnimationEffects`), `test_animation.Durations` at 200% and
50% (every duration its token times the factor), and
`shell-appearance-effects`, which checks that `Style` follows the file and
keeps the duration within its range.

### 6.6 The compositor draws the shell's colors (D7, D8, D9)

The shell already owns the theme; the compositor receives results, not inputs
to recompute them:

- **`theme <shell> <apps> <accent>`**: the accent is added. The snap preview
  uses it and becomes a rectangle inside a tree with a radius 8 shape, no
  shadow.
- **`mica <active> <inactive>`** replaces `wallpaper-tint`: the shell computes
  the bar's two colors with `micaFromTint()` in the apps' mode, as it already
  does for Settings and Files, and sends them. `decoration.c` loses
  `mica_color()`, and the server its `wallpaper_tint` and
  `has_wallpaper_tint`; without a shell the bar keeps its base grays, as today
  without a tint. One formula, in the process that owns the theme.
- **Title bar buttons.** Their hover fill is WinUI's (`subtleHover`, checked by
  §6.4's script). Each button's background is a *fading color* (from, to and a
  `vela_tween`, retargeted from its current color), ticked by
  `vela_views_tick()` under §6.2's rules: about twenty lines in `decoration.c`.
  Close's red is its hover color; the white Close glyph is a second image above
  the normal one, faded with it. No pressed color: `interact.c`
  `decoration_press()` acts on the press itself (Close closes, Minimize
  minimizes), so a pressed state would never be seen. Acting on release, as
  Windows does, would be a change of behavior, not of look, and is not part of
  this plan.

Old and new processes can meet after an update (the shell is relaunched, the
compositor isn't): an old compositor ignores the extra argument and the unknown
`mica` line, so the bar loses its tint until the next login; a new compositor
that gets no accent keeps the default.

**Verified by** functional tests: after `theme dark dark #ff0000`, dragging a
window to an edge shows a preview with the expected red at the expected alpha;
after a `mica` line, the bar has those colors; hovering Close logs a color that
changes on every frame and arrives within 120 ms plus one period at 60 and
180 Hz, and leaving halfway turns back from where it was
(`test_decorations.ShellColors`). With the real shell, Settings' bar and the
content right below it have the same color, tinted by the wallpaper
(`test_settings_bar_continues_its_content`); `scripts/check-shared-values.py`
checks the buttons' hover against `Theme.subtleHover`.

### 6.7 Blur for everyone, acrylic for Vela (D6)

| Step | Purpose | Applied to |
|---|---|---|
| dual Kawase blur | the effect the protocol asks for | every blur region |
| dither | technical: no banding when the blur is written in 8 or 10 bits | every blur region |
| saturation 1.25 | aesthetic: acrylic's recipe | Vela's surfaces |
| gray tint (`acrylic_tint`) | aesthetic, and must precede the dither (K3) | Vela's surfaces |
| translucent fill | aesthetic: the panel's own color | drawn by the client |
| Mica | aesthetic: a solid color | the title bar (K4) |
| shadows | elevation | the owner of the surface (K2) |

"Vela's surfaces" are layer surfaces whose namespace starts with `vela-`: the
shell's panels and the polkit prompt (`vela-polkit`, `vela-polkit-veil`)
already use it. Keying a visual rule on the layer namespace is what other
wlroots compositors do for blur; a third-party client that picks a `vela-`
namespace only changes its own look. When `add_surface()` finds a blur region it
sets `acrylic` on the element from `wlr_layer_surface_v1_try_from_wlr_surface()`;
otherwise `vela_pass_add_blur()` passes a zero tint and a saturation of 1. The
shell's look doesn't change.

**Verified by** a functional test with `vela-panel`, which gets a `--namespace`
option (today it always uses `vela-panel`, so the existing blur tests keep the
acrylic path): over a flat wallpaper, where the blur leaves the color unchanged, a
panel named `vela-test` shows the computed acrylic value and a panel named
`test` the wallpaper's color, both within the dither's amplitude, pixel by
pixel (`test_layers.BlurPolicy`; `vela-panel` also gets `--clear`, a 50%
black fill, and a flat `--color` wallpaper). `test_layers.py`'s blur and damage
tests pass unchanged.

### 6.8 Controls: Qt behaves, Vela looks (D12)

**Qt Quick Templates for behavior.** `QtQuick.Templates` ships with
`Qt6::QuickControls2`, already linked by `vela-controls`, Settings, Files and
the polkit prompt. It gives `T.AbstractButton`, `T.Button`, `T.ToolButton`,
`T.Switch`, `T.Slider`, `T.CheckBox`, `T.RadioButton`, `T.ItemDelegate` and
`T.TextField` with no look at all, and with: pointer and keyboard input (Space
and Enter, arrows on sliders), `hovered`, `down`, `checked`, `visualFocus`
(focus from the keyboard only), focus policy and tab order, accessible roles
and states, a `Switch.visualPosition` that follows a dragged knob, `Slider`
dragging and stepping. Vela supplies `background`, `contentItem` and
`indicator`. That removes the hand-made `Keys` and `Accessible` code and gives
the private controls that lack them keyboard and accessibility support.
Templates rather than the Basic style, because there is nothing of Basic's
look to keep; the Basic-based `TextBox` and `Choice` stay (K14). No global
custom style: Vela uses a handful of controls, and a style plugin would
restyle all of Qt's.

**One color per state combination, faded.** WinUI defines one color per state
combination, and `Theme` already has them (`control`, `controlHover`,
`controlPressed`; `subtleHover`, `subtlePressed`; `selection`,
`selectionHover`; `accentFill`, `accentFillHover`). Files' rows already pick
them by precedence (`DetailsView.qml`: drop target, then selected and hovered,
selected, hovered). So a control's background is one rectangle whose color is
that precedence, and the change fades:

```qml
// common/controls/Fill.qml: a background whose color changes fade with the
// desktop's timing. `animated: false` while a recycled delegate takes a new
// item.
Rectangle {
    property bool animated: true
    radius: Theme.radius
    Behavior on color {
        enabled: animated
        ColorAnimation { duration: Theme.fast; easing.type: Easing.BezierSpline; easing.bezierCurve: Theme.decelerate }
    }
}
```

```qml
// common/controls/Button.qml (sketch): Qt's button, Vela's look.
T.Button {
    id: control
    property bool accent: false
    property bool subtle: false
    background: Fill {
        color: control.subtle ? (control.down ? Theme.subtlePressed : control.hovered ? Theme.subtleHover : Theme.subtleRest)
             : control.accent ? (control.hovered ? Theme.accentFillHover : Theme.accentFill)
             : (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control)
        border.width: control.visualFocus ? 2 : (control.subtle ? 0 : 1)
        border.color: control.visualFocus ? Theme.focusRing : Theme.controlStroke
    }
    opacity: enabled ? 1 : 0.45
}
```

Simultaneous states compose by that precedence (pressed over hovered over
rest; selected and checked choose the family), exactly as WinUI's brushes do.
Focus and disabled change at once, as in WinUI: a disabled control must read as
disabled immediately. Interruption and reversal are `Behavior`'s: a hover that
leaves halfway turns back from the current color. `Theme.subtleRest`, a new
token, is the hover color with alpha 0 (where `"transparent"` is black), so a
fade never passes through gray.

One `Rectangle` and one `Behavior` per control: the structure Files' rows
already have, plus one object, and nothing running at rest. A theme switch fades
every visible control for 120 ms; that is the intended look, and it costs a
burst of simple color animations.

**Recycled delegates.** Files' views use `reuseItems`. A row's `Fill` sets
`animated = false` on `ListView.onPooled` (`GridView.onPooled` in the icon
view) and back to `true` on `onReused`, which Qt emits after the reused
delegate's properties have been updated: the new row takes its state without
a transition. Two lines in each of the two views.

**Where controls live.** In `Vela.Controls` only when two programs use them;
otherwise with their program, built on Templates and `Fill`:

| Control | Built on | Lives in | Replaces |
|---|---|---|---|
| `Button` (`accent`, `subtle`, `icon`, `text`; `usable` stays as an alias of `enabled`) | `T.Button` | `Vela.Controls` | the shell's ad-hoc buttons, Quick Settings' back and settings buttons, the Notification Center's `SmallButton`, notifications' close |
| `Switch` | `T.Switch`; knob on `visualPosition`, `Behavior on x` only when not `down` | `Vela.Controls` | Quick Settings' `Switch`, Settings' `Toggle.qml` |
| `Slider` | `T.Slider`; thumb size fades with hover and press; the value never animated | `Vela.Controls` | Quick Settings' `Slider`, Settings' `Slider.qml` |
| `ToolButton` (`checked`: a pane or a view that is on) | `T.ToolButton` + `Fill` | Files | Files' `ToolButton` |
| `CheckBox` | `T.CheckBox` + `Fill` | shell (Properties) | Properties' `Check` |
| `RadioButton` | `T.RadioButton` + `Fill` | Settings | the night light page's `Radio` |
| `ListItem` (`highlighted`: the chosen one; `checked`: the current one of a subtle list) | `T.ItemDelegate` + `Fill` | `Vela.Controls` | device, network and output rows; Start's power entries; the file dialogs' `Choice` |
| `Tile`, `Link` | `T.AbstractButton` + `Fill` | Quick Settings | its private `Tile` and `Link` |
| `MenuTextBox` | `TextBox` + `TextMenu` (the right-click menu `MenuTextField` has too) | shell | `FileDialogs`' `Field` |

Rows that are more than a click (a file row with its drag, drop and context
menu, a Settings card, a task card) keep their `MouseArea` and take a `Fill`
whose rest color is the hover color at alpha 0 (`Theme.subtleRest`, or the
Close red's `#00c42b1c`). A hover shown by turning a fixed color's opacity on
moves into the color, so it fades like the rest (§6.11).

**The pieces controls are made of** (PR 9). Writing Quick Settings on the
pieces above showed what every control repeats, so it is written once in
`Vela.Controls`, where both its users (the shared controls themselves and the
programs' own) can reach it:

| Piece | What it is | Was written |
|---|---|---|
| `Icon` (`name`, `size`, `onAccent`) | a theme icon at its size in the output's pixels | as an `Image` with `Theme.icons`, `encodeURIComponent` and `sourceSize`, 68 times in 34 files |
| `Eased` | a `NumberAnimation` on `Theme.decelerate`, `normal` unless given | with the curve spelled out, 53 times in 23 files |
| `ColorFade` (after PR 11's review) | a `ColorAnimation` on `Theme.decelerate` in `fast`: `Fill`'s fade, and a border's | as `ColorAnimation` with the curve spelled out, or without it, in five places |
| `FocusRing` | the keyboard focus ring, 3 units outside its parent with its corners | as a 9-line `Rectangle` in each control |
| `Chevron` (`down`, `color`) | the ">" of what opens a page, the "v" of what opens a list | as a `Shape` in Quick Settings, Task View, the Notification Center and Files' address bar |

The shell, Settings, Files and the polkit prompt take them with the rest of
their private controls (PR 10–11).

The API callers use (`text`, `accent`, `subtle`, `usable`, `clicked`) stays,
so call sites change only where a private control is replaced. Three names are
Qt's and changed with the move to Templates (PR 8): an icon is `icon.name` (the
Templates' `icon` group is final and can't become a string); `toggled()` and
`moved()` carry no argument, so handlers read `checked` and `value`; and a
binding on `checked` or `value` survives the user's input, where the
hand-made copies assigned to the property and broke it. `Slider` keeps its
`released(value)`, emitted once the user is done (released, a key, a notch of
the wheel), and `origin`, `mark` and `resetValue`, and gains `fillColor` (the
filled part and the knob's center: gray for Quick Settings' muted volume); a
double click on the knob emits `released(resetValue)` and lets the caller's
binding move the knob. A
click gives no keyboard focus (`focusPolicy: Qt.TabFocus`), as before: Tab
does, with the ring, so a click on a toolbar button doesn't take the keys from
a view. `TextBox` takes `Fill` as its background.

**Verified by** Qt Quick Test (part of Qt): for `Button`, `Switch` and `Slider`,
each state combination gives the expected color at rest; a hover reversed
halfway turns back from its current color; Space and Enter activate, Tab moves
focus and shows the focus ring, the accessible role and checked state are set;
with animation effects off the knob jumps. The programs' own controls
(`ToolButton`, `CheckBox`, `RadioButton`) get the same checks with the same
runner. For Files, a test scrolls a recycled view with selected rows and checks
that no color animation runs on a reused delegate.

### 6.9 Panels: `PanelMotion`, `Shadow`, `Dialog` (D14–D18)

**What is really shared.** Entering, leaving, turning back when reopened during
a close, and taking no input while closing: the same for Start, Quick Settings,
Notification Center, Clipboard, the OSDs, the shell's dialogs, menus, taskbar
previews, Snap Layouts, Alt+Tab and Task View. Everything else differs per
panel and stays with it: window size, page navigation, height changes, auto-hide
timers, which window to hide, what a click outside does.

**`PanelMotion.qml`** is a container: the owner puts the panel and its shadow
inside, and the container moves, scales and fades them together. A container,
because most panels clip their content (`clip: true`), so a shadow can't be the
panel's child, and a sibling anchored to the panel wouldn't follow a
`Translate`.

```qml
// shell/qml/QuickSettings.qml (sketch)
PanelMotion {
    id: motion
    x: 46; y: 46; width: 360; height: content.height
    offset: Qt.point(0, 48) // where it comes from; × Theme.motion
    onClosed: root.visible = false
    onAreaChanged: Effects.setBlur(root, [area])
    Shadow { target: panel }
    Rectangle { id: panel; anchors.fill: parent; radius: Theme.radiusOverlay; color: Theme.surface; clip: true }
}
```

Its whole interface:

| Member | Meaning |
|---|---|
| `offset` (point), `scaleFrom` (1) | where and how big the panel starts; both × `Theme.motion` |
| `duration` (`Theme.slow`) | the entering movement's; dialogs and menus pass `Theme.normal`. The fade in is always `fast`, the exit always `normal` |
| `instantExit` (false) | menus, previews, Alt+Tab, Task View (K5) |
| `open()`, `close()`, `closing`, `closed()` | `open()` during a close turns back from the current values; the content takes no input while `closing`, neither mouse nor keys (the keys go back to where they were if it turns back); `closed()` when an exit finishes and no reopening came |
| `moving`, `area` | entering or leaving; the blur region: while `moving` the rectangle covering the panel along its whole travel, at rest the panel |

No modes, no recipe table: each panel's three numbers are written where the
panel is (K8). Toasts are list items with their own add and remove transitions,
and the polkit prompt keeps its lifecycle (K13): neither uses it.

**Blur during the slide (D17).** The compositor clips the blur with the
surface's alpha (`blur_mix.frag`: full blur from alpha 0.5), so a region larger
than the panel shows nothing more where the surface is transparent; but the
panel's `Shadow` is in the same surface, and under its alpha the region blurs
too. So `area` covers the whole travel only while the panel moves, and is the
panel itself at rest: two region changes per entrance, none per frame, and a
resting shadow over the wallpaper as it is (during the slide the shadow's part
inside the travel is blurred a little, for a quarter of a second). Each panel
sets its region to `motion.area` (several panels in one window, like the
context menus, send the list of theirs). `area` doesn't change during a slide;
it follows the panel's size, on every frame of Quick Settings' height change
like Start's updates today. Start drops its per-frame updates.

**`Shadow.qml`** is `PanelShadow` moved into `Vela.Controls` (so the polkit
prompt can use it), with two depths in `Theme.shadow` (`Theme.flyout` and
`Theme.dialog` are colors): `flyout` (blur 32, offset 6, 45%) and `dialog`
(blur 48, offset 16, 45%). Start takes `flyout` (it had a
private 40/8/50%). Windows get room for the shadow: 46 units around the panel,
cut only where the window meets the taskbar. A click in that room closes the
panel, as a click outside does, through a `MouseArea` under the `PanelMotion`,
the way `ContextMenu.qml` already handles its whole window. An input region
would let the click through, but needs a new path from QML to
`QWindow::setMask()` for a band 46 units wide; consuming it is what Vela's menus
already do.

**`Dialog.qml`** is Files' dialog (the button list is the more general API) with
Settings' content slot and only `Theme` values, in `Vela.Controls`; the two
copies go. Its panel is a `PanelMotion` with `scaleFrom: 1.05` and
`duration: Theme.normal`, with the `dialog` shadow; the smoke over the window
fades with it. A button may be `enabled: false` (Settings' `primaryEnabled`);
Enter chooses the accent button only then, Esc chooses `"cancel"`, and
`close()` without a value closes it without choosing (what Settings' callers
do once the work is done elsewhere, as a paired Bluetooth device).

**How it is written** (PR 10). Every panel of the shell is a `PanelMotion`
holding its `Shadow` and its panel, and its window leaves the shadow's `reach`
around it (46 units for `flyout`, 72 for `dialog`), cut where the window meets
the taskbar or the output's edge; there the panel rests 12 units away (the
taskbar's tooltip 8). What each one passes:

| Panel | Comes from | Duration | Exit | Shadow |
|---|---|---|---|---|
| Start | 140 below, behind the taskbar | `slow` | animated | `flyout` (was 40/8/50%) |
| Quick Settings, clipboard | 48 below, behind the taskbar | `slow` | animated | `flyout` |
| Notification Center | 24 to the right | `slow` | animated | `flyout`, one per panel |
| volume OSD | 12 below | `slow` | animated | `flyout` |
| desktop OSD | fades | — | animated | `flyout` |
| Run, Confirm, Properties, the screen-share chooser, the file dialogs | scale 1.05 | `normal` | animated | `dialog` |
| Snap Layouts | 6 above, under the button | `normal` | animated | `flyout` |
| menus (shell and Files) | 8, from the side they open on | `normal` | at once | `flyout` (was 16/4/40%) |
| taskbar previews | 8 below | `normal` | at once | `flyout` (was 24/4) |
| the taskbar's tooltip | fades | — | at once | `flyout` |
| Alt+Tab | scale 0.96 | `normal` | at once | `flyout` |
| Task View (all but its backdrop, which fades with it) | scale 1.03 | `slow` | at once | — |

What writing them showed:

- **`ShadowRoom`** (`shell/qml`): the `MouseArea` under the `PanelMotion` that
  turns a press in the room into `outside()`, and lets one on the panel (or on
  any of `panels`) through. Each window says what outside means: close, or
  cancel for Confirm; the screen-share chooser does nothing, since the app is
  waiting for an answer.
- **`PanelMotion.visibleArea`**: `area` cut to the window, which every panel
  sends as its blur region. The Notification Center sends it while moving and
  its two panels at rest, so the 12 units between them aren't blurred.
- **`Shadow`'s `above` and `below`**: the Notification Center's two shadows
  stop halfway between its panels; each would otherwise fall on the other,
  which is translucent.
- **`close(true)`**: at once, without the exit. The clipboard's paste needs it:
  the compositor sends Ctrl+V 120 ms later to whoever has the keyboard, and a
  panel still leaving would have it.
- **Windows placed by the shell**: Start rests 12 units above the taskbar (it
  was 22) and, with the taskbar at the left, its window touches the output's
  edge (`atLeft`, set by `placeStartMenu`); Run and the volume OSD lose their
  layer margins, their windows touching the taskbar and the edge. Task View,
  Alt+Tab, Snap Layouts and the menus cover the output and need no room.
- **What it fixed on the way**: the clipboard rose 12 units where K8 says 48;
  Run's "Browse..." was written in Italian outside `qsTr`.

**Verified by** Qt Quick Test for `PanelMotion` (reopening during a close turns
back from the current offset and opacity and doesn't emit `closed()`; with
effects off only opacity changes; `visibleArea` is cut by the parent;
`close(true)` skips the exit) and `Shadow` (reach, cut above and below), and by
functional tests: Start's three tests in `test_shell.py` repeated for Quick
Settings, Notification Center and Clipboard; a click in a panel's shadow room
closes it, one on the panel doesn't; frames captured during a panel's entrance
have blur behind every pixel where the panel isn't fully transparent (Quick
Settings, Start and the clipboard row by row, the Notification Center column by
column); Run, Properties and the clipboard leave in `normal`, the desktop menu
and Task View at once.

### 6.10 Quick Settings (D13)

On the pieces above, in `QuickSettings.qml`:

- `PanelMotion` with `offset: (0, 48)`, cut by its window like Start; the exit
  animated;
- tiles: `T.AbstractButton` + `Fill`: turning on fades to the accent, hover and
  press fade;
- switches, sliders, buttons, rows: the shared controls;
- pages: forward slides in from the right, back from the left, `normal`, the
  distance × `Theme.motion`; written here, its only user;
- height between pages: the window grows to the larger height *before* the
  panel animates (one resize), the panel's height animates in `normal` inside
  it, and the window shrinks *after*. Two configures per page change, none per
  frame, and outside a page change the window is as small as the panel plus its
  shadow, so clicks above it reach what is below;
- the Wi-Fi password field opens with a height animation in `normal`,
  × `Theme.motion`.

How it is written (PR 9):

- **The window** is the panel plus `Shadow.reach` (46) above and to the left,
  and 12 to the right and below, where the output's edge and the taskbar cut
  the shadow. A click in that room closes the panel; one on the panel's own
  background doesn't.
- **The height** follows a target, the page's height *when its animations are
  over*: for the Wi-Fi page, the networks' list as it will be once an opening
  network has finished growing. Any change of the target (a page, a network
  opening, an app starting to play) animates the panel in `normal` and resizes
  the window at most twice: larger at once, smaller when the animation
  finishes. With animation effects off both jump together.
- **Tiles** keep their colors: the accent when on, with white 8% on hover and
  6% pressed; off, `control`, `controlHover`, `controlPressed`. Each half of a
  split tile is its own button, with Tab focus and a ring; a right click on
  either opens the tile's Settings page.
- **The sliders** are a row: a subtle `Button` (mute), the shared `Slider`
  (its `fillColor` gray when muted), the value above the knob while it's
  pressed, the level bar under the track, and the details `Button`. The wheel
  keeps the volume keys' steps (`Audio.stepVolume`), a notch at a time, so the
  `Slider`'s own wheel is off.
- **Rows** (networks, outputs, the accessibility options) are `ListItem`s; an
  open network is `highlighted`, and a click on its password field or button
  doesn't close it. Links are WinUI's HyperlinkButton, `T.AbstractButton` with
  a subtle `Fill` and `Theme.link` text. Icons, arrows, focus rings and
  animations are the pieces of §6.8.

**Verified by** the tests of §6.8 and §6.9, plus a functional test that goes to a
page and back and checks the window's height during and after the change and
the panel's size at the end. In `test_shell.py`: Start's three tests repeated
(opening and closing ten times; reopening during the exit turns back, its
window never unmapped; the surface released when hidden, already in
`test_other_panels_can_reopen_after_native_surface_release`); a click in the
shadow's room closes it and one on the panel doesn't; going to the sound page
and back, the window takes exactly two sizes, the panel passes through heights
in between, and it ends as the panel plus its room; `PanelBlur`, on a striped
wallpaper, finds blur behind every row of the panel in every frame of the
entrance, also below where it rests, and at rest a region that is the panel
only and an unblurred shadow. Each test fails with its feature taken out.

### 6.11 Keeping it so

`scripts/check-qml-motion.py`, in ctest, on the QML of shell, Settings, Files
and the polkit prompt:

1. **Durations are tokens.** `duration:` takes a `Theme` token or an expression
   of them, never a literal other than 0: otherwise the scale and the switch of
   §6.5 can't reach it.
2. **State colors fade.** A `color` or `border.color` driven by `containsMouse`,
   `pressed`, `hovered`, `down`, `checked` or `selected` belongs to a `Fill` (or
   a control built on one); on a plain `Rectangle` it is reported. So is an
   `opacity` driven by those states on a `Rectangle` or a `Fill` without its own
   `Behavior`: a hover drawn by turning a fixed color on, which jumps.

A reported line passes with a comment on it saying why (`// motion: follows the
drag`), so the exception sits next to the code; any other report fails the
check (PR 8 only reported, until PR 11 replaced the private controls). Local `Behavior`s and
animations are allowed: the taskbar indicator, the Start menu's app tiles, the
polkit prompt's shake and spinner have motion of their own.

## 7. Verification

Three kinds, kept apart:

- **Deterministic tests**, in CI, blocking: unit tests, Qt Quick Test, the
  functional tests named in §6, `scripts/test-sharpness.sh`, the damage tests,
  the two check scripts, the validation layers, the ASan build. They assert
  pixels, logged values and protocol traces, never speed.
- **Benchmarks**, on the development machine, before and after a PR, reported in
  it: `scripts/measure.sh` (idle CPU and wakeups, memory) for compositor and
  shell; `VELA_STATS` frame cost (median and p99) and missed vblanks during a
  scripted sequence (open, close, minimize, restore, maximize, desktop switch,
  snap preview, opening and closing every panel), headless at 60, 144 and 360 Hz
  and on real hardware, five runs each, compared by median; Files' cold start
  and the opening and scrolling of a 10 000-file folder. A change worse than the
  run-to-run spread needs an explanation in the PR.
- **Visual checks**, by the user on real hardware (2560×1440 at 180 Hz and 125%
  next to 1920×1080 at 75 Hz), listed per PR in §8.

The only permanent diagnostic added is `VELA_DEBUG_ANIMATION` (§6.2); the rest
are tests and existing tools.

## 8. Plan

Small PRs, each useful and revertible on its own, each updating the documents
its behavior touches. This document goes with the first.

| PR | Changes | Deterministic tests | Visual check | Documents |
|---|---|---|---|---|
| 1. Pixels | §6.1 | title identical at 40 positions × 4 scales; sharpness script | the bar at 125% while moving a window | renderer.md §3.4 (any image), §5.1 (nodes have no transform) |
| 2. Clock | §6.2 | two-output animation test; idle frame counters | a desktop switch on 180 + 75 Hz, nested and on DRM | renderer.md §4.2 (per-output sampling, wall-clock retirement); configuration.md (`VELA_DEBUG_ANIMATION`) |
| 3. Morph | §6.3 | radius log | maximize and restore | — |
| 4. Durations | §6.4 | `check-shared-values.py` | close, snap preview, magnifier | `motion.h`'s comment |
| 5. Animation effects | §6.5 | animation log with effects off; reload test | every window animation with the switch off | configuration.md (`animation-effects`) |
| 6. Colors from the shell | §6.6 | accent, `mica`, Close fade tests | bar and content seam, light and dark; hover on the bar | renderer.md §9 and §9.4 |
| 7. Blur policy | §6.7 | namespace test with `vela-panel` | shell panels unchanged; a third-party blur client | renderer.md §8.3 (policy, K3, K10) |
| 8. Shared QML pieces | `Fill`, `Button`, `Switch`, `Slider`, `Shadow`, `Dialog`, `PanelMotion`; §6.11 reporting only | Qt Quick Test for them | Settings and Files with the new `Button`, `Switch`, `Slider`, `Dialog` | development.md (where controls live) |
| 9. Quick Settings | §6.10 | §6.9 and §6.10 tests | every page, both themes, effects on and off | — |
| 10. Other panels | §6.9 for the rest | Start's tests for the edge panels; shadow-room click; blur during entrance | every panel, both themes | — |
| 11. Settings, Files, polkit, the shell's controls | the remaining private controls of §6.8, the shell's too (the Notification Center's `SmallButton`, Properties' `Check`, the file dialogs' `Field` and `Choice`, Start's power entries, the shell's `containsMouse` rows); §6.11 failing | Files reuse test; Qt Quick Test for `ToolButton`, `CheckBox`, `RadioButton`; check script | both apps and the prompt, effects on and off | development.md, testing.md |

Order: 1–3 fix the compositor without touching a color. 4 before 5 (the switch
caps the tokens) and 5 before 8 (`PanelMotion` reads `Theme.motion`). 6 and 7
are independent of each other and of the QML work. 8–11, the largest part, last.

## 9. Rejected

- **A shared animation runtime and a C catalog of named animations, with a
  `Motion` bridge to QML** (this plan's first version). It centralized the
  engine where only the numbers needed it (K1); `motion.hover` needed
  properties created at run time (a `QQmlPropertyMap`) and an animation object
  per channel, rebuilding `Behavior`; strings checked at run time replaced
  types checked at load; QML-only geometry moved into C. §6.4 keeps the useful
  part.
- **Exclusive states** ("rest", "hover", "pressed", "selected"): they can't
  express a selected row under the mouse without one state per combination.
- **Stacked translucent layers per state, each with its own opacity** (the
  second version's `Highlight`): three rectangles and three `Behavior`s per
  control, and colors that differ from WinUI's (a hover layer over a selection
  isn't `selectionHover`). One color by precedence, faded, is what `Theme` and
  Files already describe.
- **Moving the acrylic gray into the shell's fills**: it weakens the dither
  (K3).
- **A shared C header for the timings and the Mica formula**: three hops to read
  a QML token and an include path into `common/` to share four numbers; and the
  formula belongs to the shell (§6.6).
- **Three motion levels** (full, reduced, off): no user need beyond Windows'
  switch, and one more state in both processes.
- **`PanelMotion` with modes and a recipe table** (`Edge`, `Popup`, `Dialog`,
  `Overlay`, `Toast`, `Page`): six modes to express three numbers, and pages and
  toasts aren't panel lifecycles.
- **An input region for the shadow room**: a new path to `QWindow::setMask()`,
  against the click-outside behavior Vela's menus already have.
- **A global Qt Quick Controls style**: Vela uses a handful of controls.
- **Removing `fmax` alone** (the second version): it breaks the irreversible
  ends of animations (§6.2).
- **A registry of compositor animations**: five explicit calls can be read and
  grepped.
- **Transforms on scene nodes**: only snapshots scale, and resizing their pieces
  is enough.
- **Group opacity, or snapshots rendered into one texture**: an offscreen image
  per animated window per frame, or a large allocation on the first frame of
  every close, for an artifact nobody has reported (K6).
- **Compositor-drawn shadows or animations for shell panels**: the compositor
  sees a rectangle where the shell knows the panel (invariant 3).
- **Forbidding local animations in QML**: special motion would migrate into
  shared pieces that don't need it; §6.11 catches duplication instead.
- **The blurred-background cache** (renderer.md §8.3): at idle nothing is
  redrawn.

## 10. Later, when there is a reason

- **Live blur behind the title bar** (renderer.md §9.4): one flag on
  `vela_rect_node` feeding the existing `blur_box`, plus the Mica color as its
  fill. When the user wants Mica with blur.
- **Shadows for apps' own menus**: under KWin, Qt/KDE menus get theirs through
  KDE's `org_kde_kwin_shadow`; Vela doesn't implement it. A screenshot first.

## 11. Risks to check while implementing

- **The magnifier's cursor (§6.2).** The table assumes `wlr_output_cursor_move()`
  on the zoomed output is committed with that output's frame, so another
  output's tick can't show the cursor at a different time. To confirm on DRM
  with a hardware cursor; if it isn't, the magnifier's cursor is moved only in
  the zoomed output's own tick.
- **Animation start with very different leads (§6.2).** Nested, the slower
  output can fix the start a few periods ahead of the faster one, which then
  starts late instead of skipping. Acceptable in a development setup; to observe
  on real hardware.
- **Templates migration (§6.8).** `enabled` also disables focus and children,
  where `usable` didn't; a control that relied on clicks while unusable shows up
  in the tests. Keyboard focus moves differently once controls take it.
- **Theme switch (§6.8).** All visible fills fade at once: to measure on
  Settings' longest page and Files' biggest view.
- **The namespace rule (§6.7).** Every Vela surface that wants acrylic must be a
  `vela-` layer surface. True today; a future Vela app window with blur would
  get the plain one.
- **Shadow room (§6.9).** Larger transparent windows cost Qt more pixels during
  a panel's animation. PR 10's benchmark: 25 openings and closings (Start,
  Quick Settings, Notification Center, clipboard, Run), five runs each,
  headless at 100 Hz: the shell's CPU time from 0.45 s to 0.55 s (median;
  0.41–0.46 against 0.52–0.63), about 4 ms per opening and closing, the exits
  that are now animated included; the compositor's CPU time and frame cost
  unchanged (0.09 → 0.10 s, 0.32 → 0.31 ms), and nothing at idle, since a
  hidden panel gives its surface back. Start's shadow changes from its
  private recipe to `flyout`: to approve by eye.
- **Quick Settings' resize (§6.10).** The window must have grown before the
  panel does; if the configure arrives late, one frame of the growth is clipped.
  To observe at 60 Hz.
- **Version skew (§6.6).** After an update, until the next login, the bar may
  lose its tint.
