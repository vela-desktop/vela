# In-process native XWayland design

Status: **design + pure coordinate mapping prototype only** (PR #164, issue #163).
Do not advertise the native in-process renderer as implemented or enable it by default.

## Why an extra X11 bridge is insufficient

Vela's built-in wlroots XWM knows how to process and reconcile X11
maximize/minimize, Wine style transitions, fullscreen requests, unmanaged
menus and XRandR output selection. When using Xwayland Satellite, these X11
windows reach Vela as **XDG toplevels** instead: the X11->XDG translation is
controlled by Satellite, which has known mixed-DPI viewport/input-region
misalignment and window-control gaps (upstream PRs #452 and #546).

KWin's *Apply scaling themselves* and Hyprland's *force_zero_scaling*
show an alternative: keep an in-process XWM, separate the X11 pixel
coordinate system from Wayland's logical output layout, and compose X11
buffers at the correct physical resolution. X11 still has a **global** DPI;
this cannot ensure legacy apps such as Steam automatically re-layout their
own UI from 125% to 100% when dragged between monitors. The compositor must
nevertheless preserve exact pointer, window and buffer geometry.

## Contract

No game-specific environment variables or Steam launch options. The same
session DISPLAY must expose the **physical** RandR output modes while
Wayland-native applications continue using their own output scales.
Vela retains authority over X11 window management (maximize, minimize,
fullscreen, Wine properties, popups, Alt+Tab, focus, constraints, drag/drop).

Example from a real mixed-scale layout (HDMI above DP):

| Output | Physical | Wayland scale | Wayland logical region | X11 pixel region (proposed) |
|---|---|---|---|---|
| HDMI-A-1 | 1920×1080 | 1 | (0,0) – (1920,1080) | (0,0) – (1920,1080) |
| DP-3 | 2560×1440 | 1.25 | (0,1080) – (2048,2232) | (0,1080) – (2560,2520) |

The X11 output positions are **independent** of Wayland logical positions.
For each output, store a pair of origins and one scale, then map:

```text
x11.x = output.x11_origin.x + (wayland.x - output.logical_origin.x) * scale
x11.y = output.x11_origin.y + (wayland.y - output.logical_origin.y) * scale

wayland.x = output.logical_origin.x + (x11.x - output.x11_origin.x) / scale
wayland.y = output.logical_origin.y + (x11.y - output.x11_origin.y) / scale
```

The pure conversion is now in `compositor/src/x11_coordinates.{c,h}`, with
CTest coverage in `compositor/tests/x11_coordinates_test.c`. Callers
**must select the monitor in the input coordinate space first**; the simple
math module intentionally does not decide which monitor owns a point.
Do not round until an actual X11 integer configure or physical pixel
boundary must be emitted. Use consistent rounding and test boundary pixels.

## Implementation stages (all required before enabling native)

1. **Output and RandR adapter.** Compute independent X11 pixel rectangles
   from enabled physical output modes, rotation and adjacency; handle negative
   origins, partial overlap, disconnected monitors and layout reflow.
   Investigate the embedded XWayland server's output enumeration and wlroots
   integration; changing `wlr_output.scale` globally, falsifying mode
   tables without changing the render/input path, or merely assigning
   `Xft.dpi` does **not** satisfy this stage. Upstream XWayland/wlroots
   changes or a per-client protocol adapter may be required.
2. **XWM configure and window geometry.** In `xwayland.c`, convert
   `wlr_xwayland_surface_configure`, initial X11 geometry, fullscreen
   origin, configure requests, windowed restore rectangles and window
   constraints. Keep `view.c` and `output.c` in Wayland logical units
   for scene placement; size X11 content buffers in X11 physical pixels.
   Preserve the existing Wine style transition and real EWMH
   maximize/minimize handling in Vela's XWM.
3. **Input and menus.** Translate X11 pointer motion, button hit-testing,
   cursor warping, constraints, grabs, DND and X11 override-redirect popup
   positions at output changes. Surface local X11 coordinates and Wayland
   logical coordinates must map to the same visual element after crossing
   DP-3 -> HDMI-A-1. Don't patch one app's mouse event offsets.
4. **Scene, damage and fractional rounding.** Ensure the actual X11
   pixel buffer is sampled without accidental upscaling. Preserve texture
   sharpness, damage and capture regions, popup input regions and exact
   fullscreen coverage after scale changes.
5. **Session DPI policy.** A single legacy X11 server has global XSETTINGS
   / Xresources; select an explicit policy, but do **not** force a primary
   `Xft.dpi` override over a conflicting backend global scale. Apps that
   do not re-layout their UI when moved between differently scaled outputs
   cannot be promised native per-output UI sizing from a global X11 DPI.
   Per-application isolated XWayland instances or native Wayland apps are
   possible later, but not a prerequisite for correct physical game modes.

## Acceptance gate: no exceptions

- [ ] XRandR exposes DP-3 **2560×1440** and HDMI-A-1 **1920×1080**, correctly
      identifies DP-3 as primary without manual intervention.
- [ ] EU5 at fullscreen is a real compositor fullscreen window on DP-3,
      including initial launch, repeated transitions to windowed and back,
      and changes of selected game monitor.
- [ ] Steam's own maximize and minimize buttons function, independently of
      Super+Up / Super+Down; manual resize and size hints remain correct.
- [ ] At 125%, X11 pointer coordinates and displayed content match exactly;
      at 100% on the second monitor the **same** holds after moving a window.
- [ ] Steam at both displays has no blur induced by mismatched buffers or
      accidental double scaling (its UI DPI choice is tested separately).
- [ ] Hotplug, negative positions, rotations, fractional 1.25/1.5, X11
      menu popups, fullscreen overlays, clipboard/DND, Steam overlay and
      native Wayland apps pass functional GPU CI on AMD and NVIDIA.
- [ ] No per-game Steam launch flags or game-specific name matching.

## Rollout

The pure model can land independently. Keep the existing integrated
`xwayland-backend=compat` runtime while the new physical-output XWM path is
built. Keep Satellite available only as an experimental comparison, not
the mandatory solution for a working beta. Enable a native in-process
backend only **after** the complete behavior passes GPU CI and hardware
verification. PR #164 must stay draft while the remaining requirements
are incomplete.
