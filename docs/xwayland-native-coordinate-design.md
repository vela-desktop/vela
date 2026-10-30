# KWin-style XWayland scaling for Vela

Status: **WIP: a developer-only XWayland-specific xdg-output manager,
matching scene surface, damage and pointer scaling, and in-process XWM
geometry translation are implemented. Hardware verification is still
required; the stable compatibility backend is unchanged.**
Tracking: PR #164, issue #163.

## Decision: follow KWin, not Satellite

Retain Vela's in-process wlroots X11 window manager. It already implements
the EWMH fullscreen/maximize/minimize, popup and Wine style transitions that
worked correctly in Steam/EU5 with `xwayland-backend=compat`. Satellite is
retained only for experimental comparison; it must not become the release
default or mandatory runtime dependency.

Plasma/KWin's real implementation (checked against KDE/kwin main):

1. `Workspace::updateXwaylandScale` chooses a **single XWayland scale**:
   the **maximum enabled output scale** when legacy X11 applications manage
   their own scaling, or **1** when the system scales them.
2. `WaylandServer::createXWaylandConnection` gives the XWayland
   **Wayland client connection** a per-client `scaleOverride` rather than
   changing the global output scale of the compositor.
3. `XdgOutputV1Interface::sendLogicalPosition/Size` multiplies the
   `xdg-output` logical geometry **for that client** by the override.
4. `SurfaceInterface` accounts for the server scale when calculating
   logical surface size, input/opaque regions, damage and local input
   coordinates. **Buffer pixel sizes do not change.**
5. KWin's X11 window-management geometry uses centralized
   `Xcb::toXNative/fromXNative` conversions.

Relevant upstream sources:
- https://github.com/KDE/kwin/blob/master/src/workspace.cpp
- https://github.com/KDE/kwin/blob/master/src/wayland_server.cpp
- https://github.com/KDE/kwin/blob/master/src/wayland/xdgoutput_v1.cpp
- https://github.com/KDE/kwin/blob/master/src/wayland/surface.cpp
- https://github.com/KDE/kwin/blob/master/src/utils/xcbutils.cpp

**Key difference:** wlroots 0.20 does *not* expose KWin's
`ClientConnection::setScaleOverride`. Simply changing
`wlr_output_state_set_scale` affects **all** Wayland clients and is
incorrect. A Vela-owned per-Xwayland Wayland protocol adapter, or an
appropriate wlroots/XWayland change, is required. Do not pretend the
mathematical model alone enables native resolution.

## Exact KWin-style mixed-monitor coordinates

The X11 scale is **global**. With HDMI-A-1 1920×1080@100% *above* DP-3
2560×1440@125%:

| Output | Physical mode | Wayland logical rectangle | X11 rectangle at 1.25 global scale |
|---|---|---|---|
| HDMI-A-1 (100%) | 1920×1080 | (0,0) 1920×1080 | (0,0) **2400×1350** |
| DP-3 (125%) | 2560×1440 | (0,1080) 2048×1152 | (0,1350) **2560×1440** |

The HDMI virtual X11 surface is *supersampled* because its physical scale
is only 1.0. The **same multiplier** for all monitor origins and extents
keeps X11 coordinates contiguous and pointer hit testing consistent.
This is intentionally **not** the earlier independently scaled per-output
layout, which would introduce discontinuities at screen boundaries.

The compositor also updates `server->xwayland_apps_scale` on output
layout changes from the maximum enabled output scale. This is a
**diagnostic candidate only** (trace with `VELA_XWAYLAND_SCALE_TRACE=1`);
it is not passed to XWayland until the protocol and surface transforms
are complete.

The production-independent model is in
`compositor/src/x11_coordinates.{h,c}`, covered by
`compositor/tests/x11_coordinates_test.c` (CTest:
`compositor.x11-coordinates`). Both `scaled by system` (factor 1)
and `applications scale themselves` (factor 1.25) are tested with
realistic placement and roundtrip geometry.

## Implemented experimental runtime path

Vela now has a separate `x11_xdg_output.c` protocol manager whose global is
visible **only** to wlroots' integrated XWayland `wl_client`. The normal
`wlr_xdg_output_manager_v1` remains visible to all other clients.
The protocol adapter advertises the global virtual X11 position and size
at the maximum enabled output scale, and updates bound resources on output
layout changes. It is explicitly gated behind:

```sh
VELA_XWAYLAND_XDG_OUTPUT_EXPERIMENT=1
```

The Vela scene's `vela_surface_node.scale` defaults to 1.0 for every
Wayland surface. In the experiment, managed and unmanaged X11 surfaces
render at `1/xwayland_apps_scale` while their input hit-test coordinates
are multiplied by that same factor. Damage/opaque region geometry is
derived from the original client coordinates. The integrated XWM converts
configure requests, full-screen output positions and restored window
rectangles consistently between X11 and Wayland.

The experiment is **NOT enabled in Vela Settings** and must not be
mistaken for the existing `xwayland-backend=native` (Satellite). Enable
it only in headless development sessions and opt in to the strict
`tests/functional/test_xwayland_kwin.py` regression by setting
`VELA_ASSERT_KWIN_XWAYLAND=1` in the test runner's environment.

This implementation still needs build validation and multi-monitor GPU
verification of RandR modes, the fractionally-scaled scene, cursor hit
testing, Wine fullscreen, popup/subsurface positions, hotplug of already
mapped X11 surfaces and output rotation. There is **no claim that it
already fixes EU5**. Known remaining work is to update existing windows
when the global maximum scale changes and to validate the X11 root's
logical mode reconciliation with the new protocol.

## Planned Vela design

The user-facing choice, once it actually works, will match Plasma:
- **Scaled by system**: keep existing `compat` behavior, Steam window
  management and native Wayland scaling unaffected. Games currently see
  logical modes (EU5 2048×1152).
- **Applications scale themselves**: preserve the same integrated XWM but
  present a separate enlarged `xdg-output` coordinate space and adjust
  X11 surfaces/viewport, input/opaque regions, damage, popups, and EWMH
  geometry as KWin does. DP-3's X11 framebuffer can reach 2560×1440.
  This is a global X11 server choice, **not a per-game/fullscreen switch**.
- The configuration must *not* be exposed as functional until all
  paths are wired. A separate backend selection `xwayland-backend=native`
  currently refers to Satellite, NOT this in-process mode.

Implementation stages, in dependency order:
1. **Wayland-client override:** identify the XWayland Wayland client
   created by wlroots and implement `xdg-output` logical position/size
   override **only for that client**. Avoid breaking existing
   `wlr_xdg_output_manager_v1` clients and avoid global output scaling.
   Inspect supported upstream wlroots 0.20 APIs and negotiate what can be
   implemented without a fork.
2. **Surface override:** apply an XWayland-specific server scale to
   committed surface size, buffer mapping and scene dimensions, including
   the effective hit-test/input region and surface-local pointer coords.
   This is crucial for Steam on the 100% secondary; don't fix only visuals.
3. **In-process XWM geometry:** map `wlr_xwayland_surface_configure`,
   fullscreen/windowed state, minimize/maximize, popup/override-redirect,
   grabs and input to/from global physical X11 geometry. Preserve
   existing Wine style workarounds.
4. **Output/modes:** check RandR reports and fullscreen mode selection on
   the actual Xwayland binary, with and without new Xwayland 26.1 emulation.
   The server must enumerate the physical/native mode *before* a game
   enters fullscreen.
5. **User-facing setting and rollout:** wire the tested two-mode option
   to Vela Settings and config, with safe fallback and session restart
   semantics. No Steam per-game flags, no global `Xft.dpi` override,
   no `wlr_output.scale=1` hacks.

## Known limitation shared with KWin

There is only one DPI for a traditional X11 session. KWin uses a single
maximum output scale; older X11 apps that do not implement DPI self-scaling
(sometimes Steam) can appear too small or blurry on one of the monitors.
Even Plasma does **not** automatically guarantee separately correct 125% and
100% UI sizes for an arbitrary legacy client on both outputs. A future
hybrid *per application* policy would require multiple XWayland instances or
a more sophisticated isolation strategy; the Plasma-like choice is a global
tradeoff.

## Must-pass regression suite before enabling

- Native Wayland apps retain 125%/100% without geometry changes.
- Steam CSD maximize/minimize works and the pointer hits the exact element
  painted when moving between both displays.
- EU5 enumerates native 2560×1440 on DP-3 and displays true fullscreen
  with no title bar, correct input and no cropping at physical resolution.
- Switching EU5 fullscreen → windowed → fullscreen works repeatedly.
- Vela shell preference DP-3 matches XRandR primary even after hotplug.
- Input, popup, resize and pointer-lock remain correct on both displays
  and at fractional scales 1.25/1.5.
- AMD and NVIDIA GPU CI, negative layout origins, rotation, and hotplug
  tests pass. Never conflate X11 logical buffer resolution with the
  *physical* XRandR output mode.

**No release claim** until these tests pass on both headless CI and
physical hardware. PR #164 stays draft in the meantime.
