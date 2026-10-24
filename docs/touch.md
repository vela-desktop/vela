# Touch and pen

How Vela takes input from touchscreens and pens: what reaches the apps, what
Vela keeps for itself, and how the desktop works without a mouse. This document
comes before the code. It lists where Vela is today, the decisions already
taken, the invariants the code must keep, and the changes in the order they are
made, with how each is verified.

> **Status:** draft, under discussion. Nothing is implemented. The open
> questions are in §12.

The reference hardware is an ASUS ProArt laptop: a built-in touchscreen, and a
pen with 4096 pressure levels that also works on the touchpad. Every behavior
that can be simulated is covered by automated tests that run without that
hardware; what only the hardware can tell is a short manual checklist on the
ProArt (§10).

## 1. Goals

- **Touch works everywhere.** A tap does what a click does, in every app,
  Wayland or X11, and on everything Vela draws itself: the title bar, the
  borders, the shell.
- **Apps that understand touch get touch.** Scrolling, pinching and multi-finger
  input in a browser or an image viewer are the app's own, not a mouse
  imitation.
- **Nothing needs hover.** Every action that the mouse reaches by hovering has a
  touch path.
- **The system is a swipe away.** Notification Center, Task View and Start
  open from the edges of the screen, like Windows 11.
- **The pen is a pen** (goal 2): pressure, tilt, eraser and buttons reach
  drawing apps.
- **No regression** for mouse and touchpad, and no cost on machines without a
  touchscreen.

## 2. Decisions taken

1. **Touch goes to the apps.** A surface whose client listens to `wl_touch`
   receives touch. A surface whose client doesn't receives an emulated pointer
   from Vela.
2. **X11 touch is always emulated.** Xwayland apps, Wine included, always
   receive the emulated pointer for touch, even though Xwayland itself listens
   to `wl_touch`: X11 touch support is uneven, and Wine games expect a mouse.
   The pen is different (goal 2): Xwayland turns `tablet-v2` into XInput
   tablet devices attached to the core pointer, so X11 drawing apps get the
   pressure and the others still get ordinary pointer events (§6).
3. **Two goals, then one last step.**
   - **Goal 1:** touch (native and emulated), the pen *as a mouse* (no pressure),
     gestures, and a shell that works without hover (§5).
   - **Goal 2:** the pen as a pen, over `tablet-v2`, with pressure (§6).
   - **Last:** tablet mode, where the shell adapts by itself (§7).
4. **The on-screen keyboard is a separate piece of work, done first**, with its
   own design document (`docs/onscreen-keyboard.md`). It is needed here (a
   device without a keyboard cannot even unlock), and Vela wants it for other
   reasons too. What touch requires of it, and where that design covers it, is
   in §8.
5. **Convertibles use the existing services.** The tablet-mode switch comes
   from libinput, the orientation from `iio-sensor-proxy`. Vela does not read
   sensors itself.
6. **Mapping touch to external monitors is postponed.** Goal 1 covers the
   built-in panel and single-monitor setups (§5.2); external touch monitors are
   an open question (§12).
7. **The timings and the gestures are the user's.** Press and hold, double tap
   and how still a finger must be are set in Settings under Accessibility; what
   each edge and finger gesture does, in a Touchscreen page. The values in this
   document are defaults (§5.10).

## 3. Where Vela is today

- **Touchscreens are ignored.** `add_device` in `compositor/src/input.c` handles
  keyboards and pointers; touch, tablet and switch devices fall into the
  `default` case. The seat never advertises `WL_SEAT_CAPABILITY_TOUCH`, so no
  app receives touch and nothing happens when the screen is touched.
- **Touchpad gestures exist.** Three- and four-finger swipes open Task View,
  show the desktop, and switch app or virtual desktop (`vela_swipe_action`).
  The actions are reusable; only the recognition is touchpad-specific.
- **Vela draws the title bar and the borders** (`decoration.h`, `interact.h`):
  dragging, the buttons, double click and resizing are Vela's, not the app's.
  The resize band is 8 logical pixels outside the window (`BORDER_BAND`), sized
  for a mouse.
- **Snap layouts open on hover** over the Maximize button.
- **The shell is Qt Quick.** Qt listens to `wl_touch` and turns touches into
  mouse events for `MouseArea`, so most of the shell will respond to a tap as
  soon as the seat has touch. What depends on hover (tooltips, taskbar
  previews, hover states) will not.
- **The lock screen (`vela-lock`) has its own Wayland connection** and draws
  with QPainter: it does not get Qt's touch handling. It will receive the
  emulated pointer (§5.3), which is enough to tap; typing needs the on-screen
  keyboard (§8).
- **wlroots already applies rotation.** When an input device is mapped to an
  output (`wlr_cursor_map_input_to_output`), wlroots 0.20 applies the output's
  transform to touch and tablet coordinates. Vela sets rotation on the
  `wlr_output` itself, so a mapped touchscreen follows the rotation with no
  calibration matrix. An unmapped device spans the whole layout and does not.
- **Tests inject input with `vela-input`** over the virtual pointer and
  keyboard protocols, enabled by `VELA_DEBUG_INPUT=1`. There is no standard
  protocol for virtual touch or pens.

## 4. Invariants

Every change in §5–§7 follows from these.

1. **One sequence, one owner.** Who receives a touch sequence (an app over
   `wl_touch`, an app over the emulated pointer, or Vela) is decided at touch
   down and does not change. The only exception is Vela taking it back for a
   gesture, and only while the app can still be told, so that it never sees
   half a sequence as if it were complete. A `wl_touch` client gets
   `wl_touch.cancel`. An app on the emulated pointer can only be taken back
   before its button goes down: `wl_pointer` has no cancel, and a release is a
   click or a drop.
2. **A touch lands on what is shown.** The same hit test as the pointer, in the
   output's coordinates as displayed: rotation, scale and position. A touch on
   a disabled or unplugged output goes nowhere; it is never spread across the
   layout.
3. **The pointer has one owner at a time.** A real mouse button and an
   emulated press never interleave. A touch that would need emulation while a
   mouse button is held is ignored.
4. **Locked means locked.** While the screen is locked, touch reaches only the
   lock surfaces, as the pointer does, and no gesture runs. Locking ends what
   is in progress, as it already does for the mouse (`vela_lock_engage`
   clears the pointer focus): native touches get `wl_touch.cancel`; an
   emulated pointer leaves the app's surface without a release, so nothing is
   clicked or dropped; pending holds, held-back edge touches and delayed
   presses are dropped; the rest of those sequences reaches nobody, not even
   the lock.
5. **Nothing in Vela is hover-only.** Every hover action in the compositor, the
   shell and Vela's apps has a touch path.
6. **No touchscreen, no cost.** Without touch or tablet devices there are no
   listeners, timers or scene nodes for them.
7. **Test input stays behind the flag.** Injecting touch and pen input is only
   possible with `VELA_DEBUG_INPUT=1`, like the virtual pointer.

## 5. Goal 1: touch, the pen as a mouse, gestures

### 5.1 Devices and the seat

`add_device` gains touch (`WLR_INPUT_DEVICE_TOUCH`) and tablet
(`WLR_INPUT_DEVICE_TABLET`) devices, attached to the cursor so their events
arrive through `wlr_cursor` with the transform applied. The seat advertises
`WL_SEAT_CAPABILITY_TOUCH` while at least one touch device exists. Tablets do
not change the capabilities in goal 1: the pen is a pointer.

`touchscreen=no` in `vela.conf` turns the touchscreen off (§5.10).

### 5.2 Which output a device belongs to

A touchscreen or pen is mapped to one output with
`wlr_cursor_map_input_to_output`, which also gives rotation (§3). In goal 1:

- With one output, every touch and tablet device maps to it.
- A device that udev marks as built in maps to the built-in panel, the output
  that `vela_is_internal_panel` recognizes by its connector (eDP, LVDS, DSI).
  The mark is `ID_INTEGRATION=internal`, which systemd sets for I²C, SPI and
  platform devices and for USB devices on a fixed port. The bus alone is not
  enough: many built-in touchscreens are USB.
- Anything else is left unmapped and its touches are ignored (invariant 2),
  with a line in the log. External touch monitors are §12.

The mapping is redone when outputs appear, disappear or are reconfigured.

### 5.3 Native touch or emulated pointer

At touch down Vela hit-tests the point as it does for the pointer and decides
once, for the whole sequence (invariant 1):

| Under the finger | Goes to |
|---|---|
| Vela's title bar or a resize border | Vela (§5.5) |
| An X11 surface | the emulated pointer |
| A surface whose client listens to `wl_touch` (`wlr_surface_accepts_touch`) | `wl_touch`, through `wlr_seat_touch_notify_*` |
| Any other surface | the emulated pointer |

**Native touch.** Every finger is its own point, with the implicit grab that
`wlr_seat` keeps: a finger that leaves the surface keeps talking to it. A touch
down on a window activates it, like a click.

**Emulated pointer.** Only the first finger drives it: the cursor moves to the
point, the left button goes down, motion follows the finger, the button is
released at touch up or cancel. Other fingers during an emulated sequence do
not reach the app. Vela uses them for two-finger scrolling (§5.6) and
multi-finger gestures (§5.7), but only while the left button has not gone
down yet (invariant 1); once it has, they are ignored until the first finger
lifts. With `touch-hold=no` the press still waits a moment (40 ms as a
starting point), so that fingers put down together are seen as two or three
fingers, not as a click followed by a gesture. A cancel from the device itself
(palm rejection, §5.4) after the button is down can only release it.

**Drag and drop** started by touch is accepted: `wlr_seat` validates touch
serials, and the drag icon follows the finger.

### 5.4 The pen as a mouse

The pen drives the emulated pointer, never `tablet-v2` (that is goal 2):

- in proximity, hovering: the cursor follows the pen and is shown;
- tip down: left button; the eraser end behaves like the tip;
- barrel button: right button; a second barrel button: middle button;
- pressure, tilt and distance are ignored.

**Palm rejection** is libinput's. It pairs a pen with a touchscreen: the one on
the same physical device if there is one, otherwise the first it finds. While
the pen is in proximity, libinput cancels the touches already down and ignores
new ones, on the whole screen or in an area around the pen, and it accepts
touches again 90 ms after the pen leaves. wlroots passes the cancellation on
as a touch cancel, which Vela handles like any other. The ProArt checklist
(§10) verifies the pairing.

### 5.5 Vela's title bar and borders

- **Tap** on a title bar button: the button, with its pressed state.
- **Drag** on the title: moves the window, with snapping to edges and corners
  as with the mouse. The drag starts past a touch threshold, larger than the
  mouse's.
- **Double tap** on the title: maximize or restore, with its own time and a
  larger distance tolerance than double click (§5.10).
- **Press and hold** on the title or the icon: the window menu.
- **Press and hold on Maximize:** the snap layouts, which the mouse gets by
  hovering.
- **Resizing:** touches use wider bands than the mouse: 16 logical pixels
  outside the window and 24 at the corners as a starting point, to tune on the
  ProArt.

### 5.6 Press and hold, and scrolling, for emulated apps

Apps on native touch handle press and hold and scrolling themselves (Qt and
GTK do). For apps on the emulated pointer, and on Vela's own surfaces, Vela
does it:

- **Press and hold = right click.** A finger held still for the hold time
  (500 ms by default, §5.10) grows a ring under it, drawn by the compositor;
  releasing after the ring is complete sends
  a right click at that point instead of the left button. Moving past the
  tolerance before then cancels the hold and continues as a left-button drag.
  The left press is then delayed until the finger moves or the hold is
  decided, so the app sees either a left click, a drag, or a right click,
  never a mix. Taps in emulated apps therefore arrive at touch up;
  `touch-hold=no` brings them back to touch down. A tap decided at touch up
  sends the press and the release apart (16 ms as a starting point), never in
  the same frame: games that read the button state at intervals would miss the
  click.
- **Two-finger tap = right click**, at once: two fingers put down together
  (within the wait of §5.3) and lifted before the hold time without moving
  past the tolerance. Press and hold stays, as on Windows.
- **Two fingers moving together = scrolling**, sent as continuous axis events,
  in the touchscreen's natural direction.
- **No hold for games.** While the surface under the finger holds a pointer
  constraint (`pointer-constraints-v1`, locked or confined), which is almost
  always a game, the left button goes down at touch down: no hold, no wait, no
  delay. Other fingers are then ignored (§5.3), so two-finger taps, scrolling
  and multi-finger gestures don't apply there; edge swipes still do (§5.7).

### 5.7 Gestures

**Edge swipes**, like Windows 11. Each edge's action is a setting (§5.10);
the defaults:

| From the edge | Opens by default |
|---|---|
| right | Notification Center |
| left | Task View |
| bottom | Start |
| top | nothing |

Besides opening a panel, an edge can open and close the on-screen keyboard
(§8).

The edges are the output's as displayed, after rotation. Edge swipes work only
on mapped touchscreens and never while locked. A touch that starts in the edge
zone is held back, not sent: if it moves inward past the threshold it is
Vela's; if it moves along the edge, stays, or is released, it is replayed to
the app at once, with the original timestamps: apps compute a flick's speed
from them, and events squeezed into the same millisecond would look infinitely
fast. Apps never need to be cancelled for an edge swipe.

Over a fullscreen app the first swipe only shows a small tab at the edge, and a
second swipe on the tab opens the panel, so games are not interrupted by
accident. This too is a setting: the tab, the panel at once, or no edge swipes
over fullscreen apps.

**Three and four fingers** on the screen do what they do on the touchpad: up
for Task View, down for the desktop, sideways for app or desktop switching.
They have their own settings, with the touchpad's choices and defaults, so the
touchscreen and the touchpad can differ. When the fingers are recognized as
Vela's gesture, the app's fingers are cancelled (invariant 1).

**The cursor** is hidden as soon as the screen or the pen tip is touched and
shown again at the first motion of a real mouse or touchpad, or when the pen
comes into proximity or moves while hovering, as on Windows.
Native touch does not move the cursor; the emulated pointer does.

### 5.8 The shell and Vela's apps without hover

Qt reports the device behind every event, so the shell can tell a finger from a
mouse locally, without a protocol. The work is an audit of everything that uses
hover (`hoverEnabled`, `containsMouse`, `HoverHandler`, `onEntered`), with these
rules:

- **Context menus:** press and hold opens what the right button opens, on the
  taskbar, the desktop icons, Start and the Files views.
- **Taskbar:** a tap on a group with more than one window shows the previews
  that the mouse shows on hover. Tooltips don't appear for touch.
- **Notifications:** a swipe to the right dismisses a popup; a touch pauses its
  timeout the way hover does.
- **Lists and panels** scroll by flicking: Start, Quick Settings, Notification
  Center, clipboard history, Settings and Files.
- **Snap layouts** respond to a tap on each zone.

Target sizes stay as they are in goal 1. Larger targets and spacing are tablet
mode's (§7).

### 5.9 The lock screen

`vela-lock` does not listen to `wl_touch`, so it receives the emulated pointer
(§5.3): a tap shows the user and the password, as a click does. Typing the
password with touch only needs the on-screen keyboard (§8). Touches already
down when the screen locks end there (invariant 4).

### 5.10 Settings

Everything is in `vela.conf`, read like the other input settings and reloaded
with them, and shown in two places in Settings. The pages appear only when a
touchscreen exists, like the Touchpad page's "No touchpad".

**Accessibility › Touch**: how the touch is read, for people who need more
time or have less steady hands.

| Setting | `vela.conf` | Default |
|---|---|---|
| Press and hold for a right click | `touch-hold=yes` | on |
| Press and hold time | `touch-hold-time=500` | 500 ms (300–2000) |
| Double tap time | `touch-double-tap-time=500` | 500 ms (200–1500) |
| How far a finger may move and still tap, hold or double tap | `touch-tolerance=8` | 8 logical px (4–32) |
| Show where the screen is touched | `touch-indicator=no` | off |

The tolerance also sets how far a drag must go before it starts (§5.5). The
indicator draws a circle under every finger, for presentations and for people
who want to see what the screen received. It is not drawn while the screen is
locked: over the on-screen keyboard it would show the password to whoever is
watching. The press-and-hold ring reveals nothing and is still drawn.

**Touchscreen** (next to Mouse and Touchpad): what the gestures do.

| Setting | `vela.conf` | Values | Default |
|---|---|---|---|
| Touchscreen | `touchscreen=yes` | yes, no | yes |
| Right edge | `touchscreen-edge-right=notifications` | `notifications`, `quick-settings`, `task-view`, `start`, `desktop`, `keyboard`, `no` | `notifications` |
| Left edge | `touchscreen-edge-left=task-view` | same | `task-view` |
| Bottom edge | `touchscreen-edge-bottom=start` | same | `start` |
| Top edge | `touchscreen-edge-top=no` | same | `no` |
| Over fullscreen apps | `touchscreen-edge-fullscreen=tab` | `tab`, `yes`, `no` | `tab` |
| Three-finger gestures | `touchscreen-three-fingers=app` | `app`, `desktop`, `no` | `app` |
| Four-finger gestures | `touchscreen-four-fingers=desktop` | `app`, `desktop`, `no` | `desktop` |

An unknown value is read as the default, as for the touchpad's gestures.

## 6. Goal 2: the pen as a pen

Vela implements `tablet-v2` (`wlr_tablet_v2`). The decision of §5.3 extends to
the pen: a surface whose client binds `tablet-v2` for the seat
(`wlr_surface_accepts_tablet_v2`) receives the tool directly, with proximity,
tip, pressure, tilt, distance, rotation, the eraser as its own tool type, and
the buttons; any other surface keeps the emulated pointer of §5.4. Xwayland
binds `tablet-v2` too, so X11 apps get the pen (decision 2). The tool's cursor
is the one the app sets for it.

A **Pen** page in Settings (from the roadmap) holds what to do with the
buttons, which output the pen belongs to, and the pressure. libinput has no
pressure curve, only a range (`libinput_tablet_tool_config_pressure_range_set`):
where the pen starts to register and where it saturates. Vela uses the range
for that and applies its own curve (soft, linear, firm) to the normalized
value before `wlr_tablet_v2_tablet_tool_notify_pressure`.

**The pen on the touchpad** is studied here, once it is known how the ProArt
exposes it: a tablet device of its own, or the touchpad's.

## 7. Last: tablet mode

When libinput's tablet-mode switch reports tablet mode, the shell adapts by
itself, without a setting: larger targets and spacing on the taskbar and in
the panels, and the on-screen keyboard shown for every text field (not only
those touched, §8). Rotation follows `iio-sensor-proxy` while in tablet mode,
with a rotation lock in Quick Settings; automatic rotations are not saved to
`outputs.conf`.

## 8. What touch requires of the on-screen keyboard

The on-screen keyboard has its own design document,
`docs/onscreen-keyboard.md`. These requirements come from touch, and that
design covers each of them:

| Requirement | On-screen keyboard |
|---|---|
| **It appears only for touch.** It opens by itself when a text field gets the focus from a touch or a pen, never from the mouse or the keyboard, like GNOME; on a laptop with a keyboard it never gets in the way. The compositor knows which device made the last press on the seat. | §5.5, invariant 6 |
| **It can be opened by hand** from a button in the taskbar's notification area (always, only with a touchscreen, or never), with Win+Ctrl+O, or with the `keyboard` edge action (§5.10). | §5.5, §5.13 |
| **Tapping it changes nothing else.** A touch on the keyboard neither moves the focus nor closes the shell's panels or the app's menus. | §5.10, invariant 3 |
| **It works on the lock screen.** `vela-lock` implements `text-input-v3` on its own connection, and only Vela's keyboard is shown above the lock. | §5.11 |
| **In tablet mode** it appears for every text field (§7). | §5.5 |

Its stages K0–K4 come before T1. Its stage K5, showing by itself after a touch
and hiding at a physical key, can only work and be tested with touch, so it
lands together with T1.

## 9. Not in scope

- Gestures configurable per app.
- Gestures drawn by the user (shapes, custom finger counts).
- Pads with buttons and rings (Wacom-style tablets) in goal 1. They are
  ignored until goal 2.

## 10. Tests

**Unit tests (GoogleTest).** The decisions are pure functions without
wlroots, so they run on CPU-only CI: the routing table of §5.3, the
press-and-hold state machine, the edge-zone and multi-finger recognizers, the
device-to-output mapping of §5.2, and the settings of §5.10 (ranges, unknown
values, every edge action).

**Functional tests.** A private protocol, `vela-test-input-v1`, enabled only by
`VELA_DEBUG_INPUT=1` (invariant 7), creates real `wlr_touch` and `wlr_tablet`
devices inside the compositor. They go through `add_device`, the mapping and
`wlr_cursor` exactly like hardware, rotation included. `vela-input` gains
actions for it:

```
touch down ID X Y | touch move ID X Y | touch up ID | touch cancel
pen in X Y | pen move X Y | pen down | pen up | pen button N | pen out
pen pressure P | pen tilt X Y
touch device internal|external | pen device internal|external
```

Pressure and tilt are ignored until goal 2, but the protocol carries them from
T0, so that P1 does not have to change it. Test devices have no udev device,
so the protocol says whether each is built in, and `test-output add` gains a
name (`eDP-1`, through `wlr_output_set_name`) for an output standing for the
built-in panel.

A test client that listens to `wl_touch` and reports what it receives joins
the existing ones. The scenarios:

- a tap and a drag reaching a touch-aware client as `wl_touch`, with several
  fingers;
- a client without `wl_touch` and an X11 window receiving the emulated pointer;
- a tap on a 90°-rotated output landing on the right spot;
- a built-in touchscreen still mapped to `eDP-1` with a second output
  connected, and a device not built in left unmapped;
- dragging the title bar, double tap, press and hold for the window menu,
  resizing from the wider band;
- the cursor hidden on touch and shown again on mouse motion;
- press and hold sending a right click, a two-finger tap sending one at once,
  and two-finger scrolling;
- on a surface holding a pointer constraint, the button going down at touch
  down;
- a cancel from the device (palm rejection) during a native and an emulated
  sequence;
- a tap on an emulated surface arriving as a press and a release in different
  frames;
- a second finger after the emulated button is down: ignored, no scrolling;
  two fingers put down together with `touch-hold=no`: scrolling, no click;
- a mouse button held while a touch lands on an emulated surface: the touch is
  ignored and the mouse drag goes on (invariant 3);
- a longer hold time and a larger tolerance from `vela.conf` taking effect
  after a reload, and `touch-hold=no`;
- each edge swipe opening its panel; an edge set to another action, to
  `keyboard` (the on-screen keyboard opening and closing) or to `no`;
  a held-back touch replayed to the app with its original timestamps; the
  three fullscreen choices;
- three- and four-finger gestures, and the app's fingers cancelled;
- the pen as a mouse: hover, tip, barrel button;
- nothing reaching the apps or the gestures while locked; a tap on the lock
  screen showing the password;
- locking with a finger down on a native and on an emulated app: the first
  cancelled, the second left without a click, nothing more reaching either;
  the touch indicator hidden while locked;
- no touch device: no change in idle cost.

**On the ProArt.** A short manual checklist for what only the hardware can
tell: that udev marks its touchscreen and pen as built in
(`ID_INTEGRATION=internal`), that libinput pairs the pen with the touchscreen
(its debug log says so), how the pen on the touchpad is exposed, and the
thresholds and band sizes tuned by finger. A `libinput record` of the ProArt's
devices can later be replayed with `libinput replay` on another machine, to
reproduce a problem without the hardware.

## 11. Stages

Each stage is one pull request with its tests.

| Stage | Content | Sections |
|---|---|---|
| **T0** | `vela-test-input-v1`, `vela-input` touch and pen actions, the touch-aware test client | §10 |
| **T1** | Touch and tablet devices, seat capability, mapping to outputs, native `wl_touch`, cursor hiding, `touchscreen=`; with the on-screen keyboard's K5 | §5.1–§5.3, §5.7 (cursor), §8 |
| **T2** | Emulated pointer (also X11), the pen as a mouse, title bar and borders by touch, drag and drop | §5.3–§5.5, §5.9 |
| **T3** | Press and hold with the ring, two-finger scrolling, the touch indicator, Accessibility › Touch | §5.6, §5.10 |
| **T4** | Edge swipes (with the `keyboard` action), multi-finger gestures, the Touchscreen page | §5.7, §5.10 |
| **T5** | Shell and Vela's apps without hover | §5.8 |
| **P1** | `tablet-v2`, the Pen page in Settings | §6 |
| **P2** | The pen on the touchpad, if the hardware allows it | §6 |
| **M1** | Tablet mode and automatic rotation | §7 |

The on-screen keyboard's stages K0–K4 come before T1, in their own pull
requests. Touch does not need them for the ProArt, but a touch-only device
does.

## 12. Open questions

- **External touch monitors.** A USB touchscreen has no reliable link to the
  connector it is drawn on. Candidates: match the touch device's USB parent to
  the monitor's (some monitors share a USB hub), match the device's name to the
  model in the monitor's EDID, a per-device output choice in the Touchscreen
  page and in `vela.conf`, a "touch the screen you mean" calibration like
  Windows.
- **The top edge's default.** It is `no` for now. Windows uses it in
  fullscreen apps to show the title bar; Vela could add that as an action.
- **The magnifier.** With the screen magnified, a touch should land on what is
  shown, and panning needs its own gesture.
- **Default values** (hold time, tolerance, edge zone width): the defaults of
  §5.10 are to be tuned on the ProArt. They stay in logical pixels, not
  millimeters: the default scale already comes from the panel's DPI, and an
  EDID can report no physical size at all.
