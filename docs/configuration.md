# Vela configuration

Most user-facing settings should be changed through **Vela Settings**. This document describes the files and environment variables behind those controls, plus development/debug overrides that are intentionally kept out of the main README.

## Persistent compositor settings

The compositor stores non-display settings in:

```text
~/.config/vela/vela.conf
```

or, when `XDG_CONFIG_HOME` is set:

```text
$XDG_CONFIG_HOME/vela/vela.conf
```

The format is intentionally simple:

```ini
# comments begin with #
key=value
```

Vela Settings preserves unrelated lines when it updates a key and asks the compositor to reload the file. Boolean values are `yes` or `no` (`1` and `true` work too).

### `vela.conf` keys

| Key | Default | Meaning |
|---|---|---|
| `language` | system | Interface language: `it`, `en`, or empty to follow the system (Italian if the system is Italian, English otherwise). |
| `screen-off` | `10` | Minutes of inactivity before the idle action. `0` disables it. |
| `lock-on-idle` | `yes` | Lock before turning displays off. |
| `polkit-agent` | `yes` | Start Vela's polkit agent (the User Account Control dialog) in the real session. `no` leaves the job to another agent. |
| `bluetooth-adapter` | empty | The address of the Bluetooth adapter to use when there are two or more ([Bluetooth](bluetooth.md#62-the-model)), written by Settings > Bluetooth & devices > Advanced. Empty, or an adapter that isn't there: the first one. |
| `brightness` | `yes` | Drive the monitors' own brightness ([brightness](brightness.md)) in the real session. `no` starts no `vela-brightness` and shows no brightness slider. |
| `update-check` | `yes` | Check for Vela updates in the background and notify when one is available ([updates](updates.md#62-checking)), never on a metered connection. Written by Settings > System > Updates. |
| `keyboard-layout` | system/default | Comma-separated XKB layouts, for example `it,us`. |
| `keyboard-variant` | empty | Comma-separated XKB variants matching `keyboard-layout`. |
| `keyboard-options` | empty | XKB options. |
| `keyboard-repeat-delay` | `400` | Key-repeat delay in milliseconds. |
| `keyboard-repeat-rate` | `30` | Key repeats per second. |
| `night-light` | `no` | Enable Night light. |
| `night-light-temperature` | `4200` | Night light's color temperature in kelvin, 1700–6500 in steps of 100 (6500 is the screen's normal white). Without it, the older `night-light-strength` (0–100: 6500 to 1700 K) is read. |
| `night-light-nvidia-fix` | `no` | Experimental NVIDIA workaround: apply Night light in Vulkan instead of the monitor's DRM gamma LUT (may disable direct scanout). |
| `night-light-schedule` | `no` | Night-light schedule: `no`, `sunset` or `hours`. |
| `night-light-from` | `21:00` | Manual Night-light start time. |
| `night-light-to` | `07:00` | Manual Night-light end time. |
| `color-filters` | `no` | Enable the color filter. |
| `color-filter` | `grayscale` | `grayscale`, `deuteranopia`, `protanopia` or `tritanopia`. |
| `color-filters-shortcut` | `no` | Allow Super+Ctrl+C to toggle color filters. |
| `magnifier-step` | `100` | Magnifier step in percent. |
| `sticky-keys` | `no` | Sticky modifier keys. |
| `animation-effects` | `yes` | Windows and the shell's panels move as they open, close and change. `no`: nothing moves (no rise, slide, scale, flight to the taskbar or magnifier zoom) and what fades takes at most 120 ms ([visual coherence](visual-coherence.md#65-animation-effects-one-switch-d11)). Settings > Accessibility > Visual effects. |
| `animation-duration` | `100` | How long animations take, in percent of Vela's durations: 50–300 (Settings' slider, in steps of 25). With `animation-effects=no` fades stay at 120 ms. |
| `tearing` | `yes` | Allow fullscreen apps that explicitly request tearing. |
| `direct-scanout` | `auto` | `auto` (`yes` is a compatibility alias): a fullscreen app's buffer goes straight to the screen when the output allows it. `no`: always composite; use it if a game shows a black screen or garbage with the hardware cursor active. Disabling scanout also disables asynchronous tearing (`tearing=yes` only takes effect on the scanout path), and may increase game latency. Applied by `reload-config`; `VELA_SCANOUT=0` takes precedence. Settings > Display > Gaming. |
| `variable-refresh` | `games` | VRR policy: `no`, `games` or `always`. |
| `mouse-speed` | `10` | Pointer speed, 1–20. |
| `mouse-precision` | `yes` | Pointer acceleration / enhanced precision. |
| `mouse-primary-button` | `left` | Primary mouse button: `left` or `right`. |
| `mouse-scroll-lines` | `3` | Scroll lines per wheel notch. |
| `touchpad` | `yes` | Enable touchpad input. |
| `touchpad-with-mouse` | `yes` | Keep the touchpad enabled when a mouse is connected. |
| `touchpad-speed` | `10` | Touchpad pointer speed, 1–20. |
| `touchpad-tap` | `yes` | Tap to click. |
| `touchpad-natural-scroll` | `yes` | Natural scrolling. |
| `touchpad-three-fingers` | `app` | Three-finger gesture mode: `app`, `desktop` or `no`. |
| `touchpad-four-fingers` | `desktop` | Four-finger gesture mode. |

The Settings app is the preferred way to edit these values because it applies validation and reloads the compositor at the correct time.

## XWayland primary output and native-resolution games

To use native physical monitor modes for **all Steam/Proton games without any
Steam launch options**, install `xwayland-satellite` and set in
`~/.config/vela/vela.conf`:

```ini
xwayland-backend=native
```

**Restart the Vela session**, then start Steam normally; the compositor
starts a supervised Xwayland Satellite with a prebound X11 socket and exports
its `DISPLAY` to the shell and applications. All X11 applications in the
session share the same server and its XRandR primary monitor.

The default `xwayland-backend=compat` keeps wlroots' integrated XWayland,
including Vela's custom X11/window-management handling. If Satellite has not
been installed or its listening socket cannot be reserved, Vela falls back to
this compatibility backend and logs why. Note that this session-wide native
path goes through Satellite's XDG window management and may behave differently
from the integrated backend (in particular for unusual Win32 windows).


The main monitor selected in Vela Settings is also synchronized to the XRandR
primary output for X11/Wine/Proton clients. The shell re-applies it on startup,
preference changes, and monitor hotplug. This requires `xrandr` (Arch:
`xorg-xrandr`). It does not affect the Wayland-native monitor layout.

Fractional scaling is separate. At 125% scaling, Vela's built-in XWayland
currently exposes logical size 2048×1152 for a physical 2560×1440 output.
For opt-in physical-resolution enumeration, install **xwayland-satellite >=
0.6** and launch the game through:

```sh
vela-xwayland-native GAME [ARG ...]
```

For a Steam game, add the launch option:

```text
vela-xwayland-native %command%
```

To verify the actual modes exposed by the separate Satellite X server,
**do not** run plain `xrandr` in the normal shell (that queries Vela's
integrated XWayland). Run:

```sh
vela-xwayland-native xrandr --query
```

For a 2560×1440 output at 125% scale, the Satellite query must contain
**2560x1440**, not just 2048x1152. To debug a Steam launch without silently
starting in logical resolution:

```text
VELA_XWAYLAND_NATIVE_DEBUG=1 VELA_XWAYLAND_NATIVE_EXPECT=2560x1440 vela-xwayland-native %command%
```

If the mode is missing, the helper prints the actual RandR output list and
exits instead of launching the game. First confirm Satellite >= 0.6 is really
installed and bound to Vela's Wayland socket.

For fullscreen Windows games, Vela now honors explicit X11 configure requests
that target another monitor's top-left, and native XDG clients requesting
`xdg_toplevel.set_fullscreen(output)`, including X11 bridged through Satellite.
A window can also be moved to a particular enabled output without altering the
global primary display. For example, through Vela's command socket:

```sh
printf 'window active output HDMI-A-1\\n' |
    socat - UNIX-CONNECT:"$XDG_RUNTIME_DIR/vela-$WAYLAND_DISPLAY.sock"
```

This move is intentionally limited to fullscreen windows. The X11 fallback
does not reinterpret size-only configure requests as monitor changes.

The helper reserves a separate X11 display, starts Satellite, forwards the
Vela-configured primary monitor to its RandR server and runs the requested
application. It shuts down its Satellite instance after the app exits, leaving
the rest of the session on built-in XWayland. You may override the output
explicitly with `VELA_X11_PRIMARY=DP-3`.

Satellite uses the host's `wp_viewporter` support, already exposed by Vela.
It can have different window-management behavior from built-in XWayland, and
legacy X11 applications may show smaller UI text. This is **not** the same
as native zero-scaling inside Vela's integrated XWayland.

## Display configuration

Persistent display layout is stored separately in:

```text
~/.config/vela/outputs.conf
```

Each physical display gets its own section identified from make, model and serial when available:

```ini
[Vendor Model Serial]
enabled=yes
mode=2560x1440@180.000
scale=1.25
rotation=normal
position=0,0
```

Supported rotation names are `normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180` and `flipped-270`.

Files written before October 2026 used Italian names (`schermi.conf`, and Italian keys and values in `vela.conf`). Vela still reads them and rewrites them with the English names.

Normally this file should be managed through **Settings → Displays** rather than edited by hand.

Settings > System > Display lets you mark any enabled monitor as the main
display. The connector name is stored under `display/primaryOutput` in
`~/.config/Vela/vela-shell.conf`. The shell uses the selected screen for
notification popups, main taskbar/desktop icons and keyboard-invoked panels.
If unavailable, it falls back temporarily without erasing the preference,
then restores it when the monitor reconnects. The Settings button can also
pin the current Qt default as an explicit preference.

Vela keeps the *relative* monitor arrangement while shifting its global layout
so the upper-left corner of the enabled display group is always at (0,0).
For example, a 1920×1080 monitor physically **above** a 2048×1152 primary
will appear as (0,0) and (0,1080) respectively. This avoids exposing
negative monitor coordinates to XWayland/Wine while leaving the perceived
screen arrangement unchanged. Existing saved configurations with negative
positions are normalized when loaded; new positions are saved normalized.
Windows on a translated output move with it, including during Settings
preview/revert.

Settings talks directly to `wlr-output-management`; no external display utility
is required. Changes are previewed for 15 seconds. Vela's compositor keeps the
previous configuration and restores it when the deadline expires, Settings
exits or crashes, or the user chooses **Revert**. The countdown does not depend
on Settings remaining responsive. Only **Keep changes** attempts to write the new settings
to `outputs.conf`.

The preview uses `protocols/vela-output-preview-v1.xml`, bound on the same
Wayland connection as the output configuration. While a preview is pending,
competing display changes are rejected. If monitors are unplugged, restoration
covers the surviving original outputs and leaves newly connected outputs alone.
If a confirmed change cannot be saved, the active display configuration is
kept, but Settings warns that it may be lost after a restart. A failed apply
whose rollback also fails is reported distinctly.
Settings declines to apply changes on compositors that lack the preview extension.
Ordinary output-management clients retain their immediate-apply behavior when
no preview is pending. Backend failures are reported; restoration cannot guarantee
recovery from a hardware failure that also rejects the previous configuration.

## Runtime environment variables

These are useful for one-off overrides, nested sessions, experiments and troubleshooting.

### Display and input

| Variable | Effect |
|---|---|
| `VELA_SCALE` | Force output scale. A single value applies to all outputs (`1.25`); per-output values can be supplied as `DP-1=1.5,HDMI-A-1=1`. Without it, Vela chooses a scale from each monitor's DPI. |
| `VELA_OUTPUT_SIZE` | Resolution/refresh for nested or headless outputs, for example `1920x1080@144`. |
| `VELA_NATURAL_SCROLL=0` | Force classic touchpad scrolling; this override wins over the stored natural-scrolling setting. |
| `XKB_DEFAULT_LAYOUT` | Initial keyboard layout, for example `it`. |
| `VELA_TERMINAL` | Terminal launched by Super+Enter. Default: `konsole`. |

### Presentation, VRR and power

| Variable | Effect |
|---|---|
| `VELA_VRR=1` | Force VRR always on where supported. |
| `VELA_VRR=0` | Force VRR off. |
| `VELA_TEARING=0` | Disable tearing even for applications that request it. |
| `VELA_SCANOUT=0` | Disable direct scanout. Useful for A/B comparisons and debugging. |
| `VELA_READY_WAIT=0` | Apply app commits immediately even if the app's GPU has not finished drawing the buffer, so frames wait for it. Only for comparison: by default commits wait for their fences and the screen never waits for an app. |
| `VELA_LATCH=0` | Disable late latching and render when the backend frame event arrives. |
| `VELA_LATCH_MARGIN` | Minimum late-latching safety margin in milliseconds. Default: `1`; the frame clock can grow the effective margin after late frames. |
| `VELA_REALTIME=0` | Disable realtime scheduling for the compositor main thread. |
| `VELA_SCREEN_OFF` | Override idle timeout in minutes (fractions allowed). Default is 10 if no stored setting exists; `0` disables idle screen-off. Set, it also enables the idle timeout nested and headless (tests). |
| `VELA_LOCK_ON_IDLE=0` | Turn displays off on idle without locking first. |
| `VELA_LOCK` | Use another locker instead of `vela-lock`, for example `swaylock`. |
| `VELA_POLKIT_AGENT` | Start this polkit agent instead of `vela-polkit-agent`; `0` starts none. Without it, Vela's agent starts only in a real (DRM) session, never nested or headless. |
| `VELA_POLKIT_PROMPT` | For `vela-polkit-agent`: the dialog to run instead of `vela-polkit-prompt`. |
| `VELA_BLUETOOTH` | Start this Bluetooth daemon instead of `vela-bluetooth` ([Bluetooth](bluetooth.md#61-start-and-lifetime)); `0` starts none. Without it, the daemon starts only in a real (DRM) session, never nested or headless. |
| `VELA_UPDATE_DAEMON` | Start this update daemon instead of `vela-update-daemon` ([Updates](updates.md#6-vela-update-daemon)); `0` starts none. Without it, the daemon starts only in a real (DRM) session, never nested or headless. |
| `VELA_RFKILL`, `VELA_SYSFS` | For tests: a fake `/dev/rfkill` and `/sys` for turning Bluetooth on. The shell reads `VELA_RFKILL` and `VELA_SYSFS`; of the daemons, only the test build `vela-bluetooth-testing` does, never the installed one. |
| `VELA_BRIGHTNESS` | Start this helper instead of `vela-brightness` for each output (it gets `ddc <connector>` and the socket in `VELA_BRIGHTNESS_FD`); `0` starts none. Without it, helpers start only in a real (DRM) session. Either way, only DRM outputs get one. |
| `VELA_BRIGHTNESS_FAKE` | A file of fake monitors ([brightness](brightness.md#12-tests)) for `vela-brightness`: helpers then start in any session and for every output, headless included, and never touch the hardware. For tests: only builds with `VELA_BUILD_TOOLS=ON` read it; the packages' `vela-brightness` ignores it. |
| `VELA_BRIGHTNESS_SYSFS` | With `VELA_BRIGHTNESS_FAKE` only: a fake sysfs tree where the compositor looks for backlights and `vela-brightness` reads and writes them, straight into its files (never through logind). For tests, in builds with `VELA_BUILD_TOOLS=ON` only, like `VELA_BRIGHTNESS_FAKE`. |

### Appearance and shell integration

| Variable | Effect |
|---|---|
| `VELA_WALLPAPER` | Override the wallpaper image. SVG, PNG, JPEG and other supported formats can be used. |
| `VELA_ICON_THEME` | Fallback icon theme when Qt cannot resolve one. Default: `breeze-dark`. |

## Development and renderer diagnostics

These variables are for diagnosis rather than normal user configuration.

| Variable | Effect |
|---|---|
| `VELA_STATS=1` | Print per-output FPS, frame cost, draw-to-light latency and missed vblanks every two seconds. |
| `VELA_VULKAN_VALIDATION=1` | Enable Vulkan validation layers. |
| `VELA_DEBUG=1` | Enable verbose wlroots logging. |
| `VELA_DEBUG_SCANOUT=1` | Log why a fullscreen surface is not eligible for direct scanout. |
| `VELA_DEBUG_DAMAGE=1` | Force full-output damage every frame, bypassing normal damage reduction for diagnosis. |
| `VELA_DEBUG_ANIMATION=1` | Log, per output and frame, the presentation and wall times and each running animation's progress and geometry (position, scale, opacity), and a line when an animation retires (`tests/functional/test_animation.py` reads it). |
| `VELA_DEBUG_SYNC=1` | Make the CPU wait for the GPU on every renderer submission; useful when isolating synchronization problems. |
| `VELA_DEBUG_LINEAR=1` | Force linear render buffers instead of normal tiled/modifier allocation; useful for allocator/modifier diagnosis. |
| `VELA_DEBUG_INPUT=1` | Enable the virtual keyboard/pointer protocol used by `vela-input`. Off by default for security. |
| `VELA_FILES_TIMING=1` | Make `vela-files` print startup timing data; used by `scripts/measure.sh`. |
| `VELA_SCREEN_DUMP_OUTPUT` | Select which output `vela-screen-dump` captures by zero-based index. |

## Internal variables

The supervisor, session launcher and test harness also use internal variables such as `VELA_WAYLAND_SOCKET_FD`, `VELA_WAYLAND_DISPLAY`, `VELA_BRIGHTNESS_FD`, `VELA_START_LOCKED`, `VELA_RESTARTED`, `VELA_BUILD` and `VELA_TEST_MARK`.

They are implementation details, are not intended as stable user configuration, and may change without notice.

## Examples

Run Vela nested at 1440p/180 Hz with 125% scale:

```sh
VELA_OUTPUT_SIZE=2560x1440@180 VELA_SCALE=1.25 sh scripts/run-nested.sh
```

Compare composited fullscreen rendering against direct scanout:

```sh
VELA_SCANOUT=0 VELA_DEBUG_SCANOUT=1 sh scripts/run-nested.sh
```

Run with Vulkan validation and renderer statistics:

```sh
VELA_VULKAN_VALIDATION=1 VELA_STATS=1 sh scripts/run-nested.sh
```

Force full damage while diagnosing rendering artifacts:

```sh
VELA_DEBUG_DAMAGE=1 sh scripts/run-nested.sh
```
