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
| `keyboard-layout` | system/default | Comma-separated XKB layouts, for example `it,us`. |
| `keyboard-variant` | empty | Comma-separated XKB variants matching `keyboard-layout`. |
| `keyboard-options` | empty | XKB options. |
| `keyboard-repeat-delay` | `400` | Key-repeat delay in milliseconds. |
| `keyboard-repeat-rate` | `30` | Key repeats per second. |
| `night-light` | `no` | Enable Night light. |
| `night-light-strength` | `48` | Night-light strength, 0–100. |
| `night-light-nvidia-fix` | `no` | Experimental NVIDIA workaround: apply Night light in Vulkan instead of the monitor's DRM gamma LUT (may disable direct scanout). |
| `night-light-schedule` | `no` | Night-light schedule: `no`, `sunset` or `hours`. |
| `night-light-from` | `21:00` | Manual Night-light start time. |
| `night-light-to` | `07:00` | Manual Night-light end time. |
| `color-filters` | `no` | Enable the color filter. |
| `color-filter` | `grayscale` | `grayscale`, `deuteranopia`, `protanopia` or `tritanopia`. |
| `color-filters-shortcut` | `no` | Allow Super+Ctrl+C to toggle color filters. |
| `magnifier-step` | `100` | Magnifier step in percent. |
| `sticky-keys` | `no` | Sticky modifier keys. |
| `tearing` | `yes` | Allow fullscreen apps that explicitly request tearing. |
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
| `VELA_SCREEN_OFF` | Override idle timeout in minutes. Default is 10 if no stored setting exists; `0` disables idle screen-off. |
| `VELA_LOCK_ON_IDLE=0` | Turn displays off on idle without locking first. |
| `VELA_LOCK` | Use another locker instead of `vela-lock`, for example `swaylock`. |
| `VELA_POLKIT_AGENT` | Start this polkit agent instead of `vela-polkit-agent`; `0` starts none. Without it, Vela's agent starts only in a real (DRM) session, never nested or headless. |
| `VELA_POLKIT_PROMPT` | For `vela-polkit-agent`: the dialog to run instead of `vela-polkit-prompt`. |

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
| `VELA_DEBUG_SYNC=1` | Make the CPU wait for the GPU on every renderer submission; useful when isolating synchronization problems. |
| `VELA_DEBUG_LINEAR=1` | Force linear render buffers instead of normal tiled/modifier allocation; useful for allocator/modifier diagnosis. |
| `VELA_DEBUG_INPUT=1` | Enable the virtual keyboard/pointer protocol used by `vela-input`. Off by default for security. |
| `VELA_FILES_TIMING=1` | Make `vela-files` print startup timing data; used by `scripts/measure.sh`. |
| `VELA_SCREEN_DUMP_OUTPUT` | Select which output `vela-screen-dump` captures by zero-based index. |

## Internal variables

The supervisor, session launcher and test harness also use internal variables such as `VELA_WAYLAND_SOCKET_FD`, `VELA_WAYLAND_DISPLAY`, `VELA_START_LOCKED`, `VELA_RESTARTED`, `VELA_BUILD` and `VELA_TEST_MARK`.

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
