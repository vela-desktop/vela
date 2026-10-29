# Developing Vela

This document covers the day-to-day development workflow for Vela: building, running nested or headless sessions, the repository layout, and the tools used to inspect a live compositor.

For renderer design and frame scheduling, see [renderer.md](renderer.md). For the complete test setup, see [testing.md](testing.md). Runtime overrides and debug switches are documented in [configuration.md](configuration.md).

## Build

Vela requires CMake 3.22 or newer, Ninja, a C17 and C++20 compiler, wlroots 0.20, Qt 6.7 or newer with its Linguist tools, LayerShellQt, Vulkan headers and loader, `glslc`, GBM, libdrm, FreeType, HarfBuzz, Fontconfig and PAM. librsvg is optional (app icons in the title bar). The development tools and tests also need zlib, GoogleTest and Python 3. Distribution package lists are below in [distribution dependencies](#distribution-dependencies).

## Distribution dependencies

These packages are for **building Vela from source**, not the runtime requirements of the [user installer](installation.md). Arch `vela-git` deliberately installs Core and Desktop together: Konsole, Breeze icons, `xdg-desktop-portal` and the wlr screen-capture backend are default dependencies. `xdg-desktop-portal-gtk` is installed for generic application portals, while `xdg-desktop-portal-kde` and Plasma Workspace are not required. Optional extras include KDE Connect and BlueZ for Bluetooth. `vela-update --build` installs `base-devel` through pacman only when a local build is required; makepkg installs the selected PKGBUILD's other build dependencies. `git`, `curl`, `sudo` and `util-linux` are runtime dependencies of the updater. Keep them synchronized with the CI's build dependencies.

<details>
<summary><b>Arch / CachyOS / EndeavourOS</b></summary>

```sh
sudo pacman -S --needed base-devel git cmake ninja pkgconf wlroots0.20 wayland wayland-protocols \
    libxkbcommon pixman libinput qt6-base qt6-declarative qt6-svg qt6-wayland qt6-tools qt6-multimedia layer-shell-qt \
    vulkan-headers vulkan-icd-loader shaderc mesa libdrm freetype2 harfbuzz fontconfig cairo \
    pam polkit librsvg zlib gtest python python-numpy python-pillow hicolor-icon-theme shared-mime-info xorg-xwayland \
    pipewire wireplumber systemd-libs
```
</details>

<details>
<summary><b>Fedora</b></summary>

```sh
sudo dnf install git cmake ninja-build gcc gcc-c++ pkgconf-pkg-config \
    wlroots-devel wayland-devel wayland-protocols-devel libxkbcommon-devel \
    pixman-devel libinput-devel libdrm-devel mesa-libgbm-devel \
    qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtdeclarative-devel \
    qt6-qttools-devel qt6-qtsvg-devel qt6-qtwayland-devel layer-shell-qt-devel \
    vulkan-headers vulkan-loader-devel glslc freetype-devel harfbuzz-devel \
    fontconfig-devel pam-devel polkit-devel librsvg2-devel cairo-devel \
    zlib-devel gtest-devel python3 python3-numpy python3-pillow shared-mime-info \
    pipewire-devel wireplumber-devel pipewire pipewire-utils wireplumber systemd-devel
```
</details>

<details>
<summary><b>Ubuntu 26.10 (experimental native DEB updater and CI validation)</b></summary>

Use Ubuntu 26.10 (Stonking Stingray) with the `universe` repository enabled.
The CI uses the official `ubuntu:26.10` container and compiles the compositor,
shell, lock screen, Settings, Files, polkit agent, reporter, and development
tools. It also builds an experimental .deb and checks installation and removal in a
clean Ubuntu 26.10 container. It does not validate a graphical login session or
GPU/DRM functionality.

```sh
sudo apt update
sudo apt install --no-install-recommends \
    ca-certificates git build-essential cmake ninja-build pkgconf \
    libwlroots-0.20-dev libwayland-dev wayland-protocols libxkbcommon-dev \
    libpixman-1-dev libinput-dev libgbm-dev libdrm-dev \
    libvulkan-dev glslc libfreetype-dev libharfbuzz-dev libfontconfig-dev \
    libcairo2-dev librsvg2-dev libpam0g-dev libpolkit-agent-1-dev \
    libpolkit-gobject-1-dev zlib1g-dev libgtest-dev libsystemd-dev \
    qt6-base-dev qt6-base-private-dev qt6-declarative-dev \
    qt6-svg-dev qt6-wayland-dev qt6-tools-dev qt6-tools-dev-tools \
    qt6-l10n-tools liblayershellqtinterface-dev \
    libpipewire-0.3-dev libwireplumber-0.5-dev \
    dpkg-dev file python3 python3-numpy python3-pil dbus
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DVELA_BUILD_COMPOSITOR=ON -DVELA_BUILD_SHELL=ON \
    -DVELA_BUILD_POLKIT=ON -DVELA_BUILD_REPORT=ON -DVELA_BUILD_TOOLS=ON
cmake --build build --parallel 2
ctest --test-dir build -N -LE 'gpu|distro-packaging'
ctest --test-dir build --output-on-failure -LE 'gpu|distro-packaging'
```

Ubuntu 26.10 also has an [experimental DEB updater](installation.md#ubuntu-2610-experimental-native-deb-updater).
Build success alone does not prove Ubuntu graphical session functionality:
validate Wayland, portals, audio, polkit and GPU-specific behavior on real hardware.

</details>

<details>
<summary><b>openSUSE Tumbleweed (experimental; not currently validated by CI)</b></summary>

```sh
sudo zypper install cmake ninja gcc-c++ wlroots-devel wayland-protocols-devel \
    libxkbcommon-devel pixman-devel libinput-devel qt6-declarative-devel qt6-linguist-devel \
    qt6-svg-devel qt6-waylandclient-devel layer-shell-qt6-devel \
    vulkan-headers vulkan-devel shaderc libgbm-devel libdrm-devel \
    freetype2-devel harfbuzz-devel fontconfig-devel pam-devel librsvg-devel \
    zlib-devel gtest python3
```
</details>

Optional: `qt6-multimedia` (Fedora: `qt6-qtmultimedia-devel`) plays videos and sounds in File Explorer's quick preview (Space); without it they show as an icon.

## Build commands

The normal development build is:

```sh
cmake -B build -G Ninja
cmake --build build
```

Without an explicit build type, Vela defaults to `RelWithDebInfo`.

Useful CMake options:

| Option | Default | Purpose |
|---|---:|---|
| `VELA_BUILD_COMPOSITOR` | `ON` | Build the Wayland compositor and session integration. |
| `VELA_BUILD_SHELL` | `ON` | Build the Qt Quick shell, Settings, Files and lock screen. |
| `VELA_BUILD_TOOLS` | `ON` | Build development tools and enable the CTest suites. |
| `VELA_BUILD_REPORT` | `ON` | Install the standalone diagnostic reporter; requires Python 3.10 or newer. |
| `VELA_REPORT_GUI` | Same as `VELA_BUILD_SHELL` | Build the ordinary Qt Widgets reporting window, also usable in other desktops. |
| `VELA_WLROOTS` | `wlroots-0.20` | pkg-config name used for wlroots. Only wlroots 0.20 is supported. |

For example:

```sh
cmake -B build -G Ninja -DVELA_BUILD_TOOLS=ON
cmake --build build
```

## Run Vela nested

The fastest way to work on Vela without ending the current Wayland session is:

```sh
sh scripts/run-nested.sh
```

Vela opens as a window that behaves like a virtual monitor. If `build/shell/vela-shell` exists, the script starts it together with the compositor.

Files and Settings are launched from the same build tree when available, even if an
older Vela package is installed in `/usr/bin`. This includes opening them from
the Start menu, desktop shortcuts, and shell actions. The installed executables
are used only when no matching build-tree executable exists.

The host compositor normally owns the Super key, so Vela exposes Alt-based alternatives while nested. The full mapping is in [shortcuts.md](shortcuts.md).

A different build directory can be supplied as the first argument:

```sh
sh scripts/run-nested.sh build-debug
```

## Run Vela headless

For automated testing and compositor work that does not need a visible window:

```sh
sh scripts/run-headless.sh
```

This starts wlroots' headless backend, disables physical libinput devices, enables Vela's debug input protocol, and creates a virtual output. The log prints the `WAYLAND_DISPLAY` selected for the session.

A typical inspection workflow is:

```sh
sh scripts/run-headless.sh &
export WAYLAND_DISPLAY=wayland-1

build/tools/vela-input key super+Left
build/tools/vela-windows list
build/tools/vela-screen-dump screen.png
```

The exact display name may differ; use the one printed by the compositor.

## Development tools

The screen capture CLI, `vela-screen-dump`, can also be built without the compositor,
Qt shell, GPU test clients, or polkit agent:

```sh
cmake -S . -B build-capture -DVELA_BUILD_CAPTURE_TOOLS=ON \
    -DVELA_BUILD_TOOLS=OFF -DVELA_BUILD_COMPOSITOR=OFF \
    -DVELA_BUILD_SHELL=OFF -DVELA_BUILD_POLKIT=OFF -DVELA_BUILD_REPORT=OFF
cmake --build build-capture --target vela-screen-dump
```

### Capture memory and permission boundaries

The C core freezes width and height at SHM allocation. Late size changes
invalidate the pending frame rather than rewriting its mapping metadata.
Both C and Qt consumers check dimensions, stride and allocation length before
reading pixels. Captures are one-shot; callers must end or cancel jobs before
destroying the Wayland connection, and dispatch that connection on one thread.
`vela-screen-record` is the exception: it keeps its own session, with two
buffers, for as long as it records. It selects outputs by their stable
`wl_output.name` (`VELA_SCREEN_RECORD_OUTPUT=HEADLESS-1`) or a legacy numeric
registry index, bounds repeated capture errors, and refuses to overwrite
existing recording files. The output directory must already exist and the
`frames.raw` and `frames.txt` files must not.

Both the legacy `wlr-screencopy` protocol and the new
`ext-image-copy-capture` protocol expose the **composited output**. On
lock, Vela disables normal desktop scene layers and renders the lock surface;
either protocol must show that lock surface, not an underlying window.
Direct Wayland clients in the same unlocked session can request screenshots
without a portal prompt; this change does not introduce per-app authorization.

Foreign-toplevel window capture is different: it draws a window in isolation,
including one hidden behind other windows. Vela now rejects these requests
while locked and revokes pre-existing capture sources as the lock engages.
Check both newly requested and already-active sources on real hardware.
The current frontends support only normal-transform output frames.

The shared `vela-capture` static library requires Wayland client headers,
wayland-protocols (build time), and the Wayland scanner. The CLI additionally
needs zlib for PNG output. These dependencies do not make Qt, wlroots or Vulkan
requirements for this standalone target. `VELA_BUILD_GPU_TEST_TOOLS=OFF` disables
the special Vulkan test client in full development builds; functional GPU tests
that require that client must not be run in that configuration.

The tools under `build/tools/` are small clients intended for development and functional tests.

| Tool | Purpose |
|---|---|
| `vela-input` | Inject keys, clicks, drags and typed text when `VELA_DEBUG_INPUT=1`. |
| `vela-windows` | Inspect windows and compositor-visible state. |
| `vela-screen-dump` | Capture an output or region to PNG. |
| `vela-screen-record` | Record an output or region frame by frame, each with its presentation time, until SIGINT (`frames.raw` and `frames.txt` in a directory; `VELA_SCREEN_RECORD_DOWNSCALE=2` for full outputs at high refresh rates). |
| `vela-pattern` | Produce deterministic visual test content (`--decorated` asks for Vela's title bar, `--popup X,Y` opens a 200×150 magenta menu anchored at that point of the window). |
| `vela-panel` | Fake shell pieces on wlr-layer-shell: `wallpaper`, and `taskbar [--blur]` with a reserved area and background blur. |
| `vela-x11` | An X11 app (through Xwayland) for the X11 tests: an orange 300×200 window with class `vela.x11` that draws only on Expose, like most apps; `--menu X,Y` adds an override-redirect menu; `--wine-backdrop` adds Wine's black fullscreen backdrop (override-redirect, then managed with fullscreen, skip-taskbar and below). |
| `vela-x11-present` | An X11 window that shows its frames with Present, like games: an orange pixmap until Xwayland flips it, then, after a line on stdin, a green one with an empty update region. |
| `vela-randr` | Configure outputs like Settings > Display (wlr-output-management): list them, or `--output NAME` with `--on`/`--off`, `--scale`, `--pos X,Y`, `--mode WxH`. |
| `vela-testlock` | Test the lock path without requiring a password; it unlocks itself. |
| `vela-slowgpu` | An app whose GPU finishes each frame late (Vulkan compute job, explicit or `--implicit` sync), to check that the screen never waits for it. |

The compositor also exposes a command socket at:

```text
$XDG_RUNTIME_DIR/vela-$WAYLAND_DISPLAY.sock
```

It accepts one command per line. Headless tests use commands such as:

```text
state
test-output add 2560x1440
test-output remove HEADLESS-2
test-power off
test-power on
reload-config
caffeine on [minutes]
caffeine off
caffeine toggle
caffeine status
```

`state` returns the compositor's visible state as JSON and is the primary assertion surface used by the functional test harness.

### Before/after videos

`scripts/record-before-after.py` records the same scene on two builds and puts
the takes side by side, for the journal on velade.org:

```sh
cmake --build build --target vela-screen-record
scripts/record-before-after.py --before f72a182^ --after main --pick 3fb2a40 --out ~/videos
scripts/record-before-after.py --list
```

It builds each ref in a worktree under `~/.cache/vela-before-after`, runs each
scene headless with the real shell, as the functional tests do, and records it
with this checkout's `vela-screen-record`. The takes are lined up on their
first new image after the scene starts and encoded by FFmpeg as WebM, MP4 and
a PNG poster. Each scene also writes a `SCENE-manifest.json` alongside the
videos, recording exact source commits, requested picks, output name and size,
capture/presentation counts and pixel format. Cached worktrees are verified
against their original commit and picked diff before reuse; an interrupted
cherry-pick is discarded and rebuilt. `maximize` runs without late latching (`VELA_LATCH=0`): with
it, the two monitors' leads differ by about 1 ms and the old shared clock
hardly showed (see D3 in [visual-coherence.md](visual-coherence.md)). Scenes
about frame pacing (`maximize`) also give a strip of consecutive refreshes,
before above and after below (`maximize-frames.png`), and say how many repeat
the previous image. It needs
a GPU, `dbus-daemon`, `ffmpeg`, PipeWire with WirePlumber and unprivileged
user namespaces. Both the session and system D-Bus addresses point to the
take's private bus; NetworkManager, BlueZ and other host services are not
available to the shell, so real network names and device identities cannot
appear in Quick Settings (those devices will show as unavailable). The text
is in English, sound devices are made up on a private PipeWire, and Settings
sees a user named Alex.

For each take it prints the frames captured and the frames presented. Frames
where only the pointer moved are presented but not captured, since captures
leave the pointer out; otherwise fewer captured means the video misses some.
Before #111 (`3fb2a40`), copying a 1080p output into shared memory took Vela
longer than a frame at 180 Hz, so recording a build without it slows that
build down. `--pick COMMIT` applies a commit to both builds when they don't
already have it, uncommitted, in their own worktree.

### Wine/Proton fullscreen style transitions

Wine/Proton exports its native Windows window styles through
`_WINE_HWND_STYLE`. A fullscreen Windows popup can turn into a captioned
overlapped window even when Wine does not issue an EWMH fullscreen change.
Vela observes those property notifications on managed X11 windows and
reconciles its fullscreen state after an observed `WS_POPUP →`
decorated style transition while already fullscreen. The reverse change
re-enters fullscreen **only when it undoes that specific automatic exit**:
a popup window by itself never implies fullscreen. Actual fullscreen
state changes clear that restore hint, while redundant requests which
reaffirm the existing state do not. The armed return also survives
intermediate non-popup Wine styles. Non-Wine windows, other style changes,
and unknown initial styles are left alone. Property replies are polled
asynchronously to avoid blocking the compositor's event loop.

The X11 geometry must agree with Vela's scene: when transitioning into
fullscreen, maximizing, or restoring, Vela now places the scene node *before*
calling `vela_view_configure_size` so the X11 configure request contains the
**final position and size together**, avoiding a stale root position. The
functional X11 helper can independently query root-relative geometry to detect
a mismatch after either direction of a mode change.

This also updates the window's size and scene layer through the ordinary
`vela_view_set_fullscreen` path.

The compatibility handling is based on the
documented Windows style flags, not game names or opaque pixel inspection.
The isolated `test_x11.py` tests exercise both transitions, fullscreen
at initial map, popup windows that must stay windowed, deleted style
properties and unrelated style changes.

### Wine/Proton fullscreen backdrop

Wine puts a black, screen-sized window with the game's `WM_CLASS` under a
fullscreen game. It is created override-redirect and made managed later,
then asks for `_NET_WM_STATE_FULLSCREEN`, `SKIP_TASKBAR`, `SKIP_PAGER` and,
after mapping, `BELOW`. Vela follows wlroots' `set_override_redirect`: an X11
window that changes kind is recreated as a view or as an unmanaged surface,
keeping its surface and map state (otherwise the backdrop stays an
override-redirect surface above the game). Any X11 view with
`_NET_WM_STATE_BELOW` stays at the bottom of its layer (on map, on
fullscreen changes and when the state arrives later), never takes the
keyboard and is left out of Alt+Tab; `SKIP_TASKBAR` views get no taskbar
handle. `test_x11.py` reproduces the sequence with `vela-x11 --wine-backdrop`.
Even if a mapped BELOW backdrop switches back to override-redirect, it stays
in the ordinary window layer below fullscreen rather than in the X11 popup
layer above fullscreen. Other override-redirect menus retain their normal
popup stacking. `--backdrop-id-file PATH` exposes the backdrop XID for the
mapped transition regression test.
The mapped-state regression tests also use `vela-x11 --override-redirect XID on|off`
and `vela-x11 --state XID below|skip-taskbar on|off` to exercise transitions
without first unmapping the X11 window. Switching to override-redirect does
not create a close-animation snapshot: the same surface remains visible.

## Project layout

```text
compositor/src/     compositor core, in C (see c-core.md)
  scene/            scene graph, flattening, occlusion and damage
  render/           Vulkan renderer, allocation, synchronization and shaders
  supervisor.c      Wayland socket holder and compositor crash recovery
  main.c, server.c  arguments, startup, main loop, shutdown, animations
  command.c         command socket and the JSON state it answers with
  session.c         session environment (systemd, D-Bus) and hooks
  output.c          outputs, frame scheduling, refresh rate, VRR and power
  output_config.c   outputs.conf: what the user chose for each monitor
  output_manager.c  wlr-output-management: monitor changes from tools
  nested.c          Vela in a window of another session (scale, shortcuts)
  view.c            windows (xdg-shell), taskbar handles, open animation;
                    xwayland.c for X11 windows
  popup.c, layer.c  menus and shell pieces (wlr-layer-shell)
  decoration.c      Vela's title bar
  snapshot.c        frozen window images for close/minimize/maximize animations
  focus.c           keyboard focus between windows and shell pieces
  interact.c        pointer on windows: move, resize, title bar, keyboard move
  bindings.c        shortcuts, window menu and window actions
  switcher.c        Alt+Tab
  snap.c            snap, Snap Assist, snap layouts and snap groups
  workspace.c       virtual desktops
  a11y.c            night light, color filters, magnifier and sticky keys
  input.c           seat, pointer, touchpad, gestures, pointer constraints
  keyboard.c        keyboards, layouts and key handling
  shell.c           the supervised Qt shell and messages to it
  lock.c            session lock and idle handling

compositor/tests/   compositor unit tests
shell/              Qt Quick shell: taskbar, launcher, panels and desktop
explorer/           vela-files
settings/           vela-settings
lock/               vela-lock
polkit/             vela-polkit-agent and vela-polkit-prompt (docs/polkit-agent.md)
common/             Vela.Controls: theme, controls and blur shared by the Qt Quick apps;
                    audio/: vela-audio, the shell's and Settings' audio backend (docs/audio.md)
session/            login entry, systemd target and portal integration
packaging/          cross-distro vela-update and shared updater tests
packaging/arch/     Arch Linux PKGBUILD
packaging/fedora/   Fedora RPM spec and RPM-specific tests
report/             vela-report: shared Python core/CLI and standalone Qt Widgets GUI
tools/              development and test clients
tests/functional/   headless functional scenarios
tests/audio/        private PipeWire and WirePlumber for the audio tests (docs/audio.md)
scripts/            nested/headless launchers, sharpness and measurements
docs/               design and developer documentation
```

## Translations

The interface is in English and Italian. Strings in the code are written in English and wrapped for translation: `qsTr()` in QML, `QCoreApplication::translate()` (or `QT_TRANSLATE_NOOP` for tables) in C++. Italian lives in one Qt Linguist file per app:

| App | Translation |
|---|---|
| Shell | `shell/i18n/vela-shell_it.ts` |
| Files | `explorer/i18n/vela-files_it.ts` |
| Settings | `settings/i18n/vela-settings_it.ts` |
| Lock screen | `lock/i18n/vela-lock_it.ts` |
| User Account Control (polkit) | `polkit/i18n/vela-polkit-prompt_it.ts` |

The build compiles them and embeds them in each executable. After adding or changing text, refresh the files and translate the new entries (they are marked unfinished), for example with Qt Linguist:

```sh
cmake --build build --target update_translations
linguist settings/i18n/vela-settings_it.ts
```

The language is chosen in Settings → Time & language (`language=` in `vela.conf`, see [configuration.md](configuration.md)). Every app watches that file and retranslates itself without restarting (`shell/src/language.h`). Dates, months and weekdays follow the chosen language; number formats follow the system's region.

Two habits keep translation working:

- never compare a label to find a menu entry or a state: compare an identifier, or compare against the same `qsTr()` string;
- when one English string needs two different Italian translations in the same file (Italian genders: "consigliato" for a scale, "consigliata" for a resolution), give each a disambiguation (`qsTr(" (recommended)", "scale")`).

## Architecture boundaries

Vela deliberately uses wlroots for low-level Wayland, DRM/KMS, libinput and backend plumbing while keeping the parts that define the desktop's rendering behavior in Vela itself.

The compositor owns its scene graph, Vulkan renderer, frame clock, damage handling, direct-scanout policy, window-management policy and compositor-side decoration path. The shell is a separate Qt Quick process, so a shell crash does not take client windows down with it.

The renderer design is documented in detail in [renderer.md](renderer.md).

## Controls

Vela's Qt Quick interfaces share one module, `Vela.Controls` (`common/controls`): `Theme` (colors, radii, shadows, durations) and the pieces more than one program uses. Their behavior is Qt's (Qt Quick Templates: input, keyboard, focus, accessibility); their look and their timing are Vela's ([visual coherence](visual-coherence.md#68-controls-qt-behaves-vela-looks-d12)).

| Piece | What it is |
|---|---|
| `Fill` | A background whose color, chosen by the control from its state, fades in `Theme.fast` |
| `Icon`, `Chevron`, `FocusRing`, `Eased`, `ColorFade` | What controls are made of: a theme icon at its size, the ">" and "v" arrows, the keyboard focus ring, a number and a color animated on Vela's curve |
| `Button`, `Switch`, `Slider` | Templates' controls on `Fill`; `usable` is an alias of `enabled`; a subtle `Button` that is `checked` (what it opens is open) rests on the hover color |
| `ListItem` | A row of a list on `Fill`: subtle (resting on the hover color when `checked`, the current one), or the control's colors when `highlighted` |
| `TextBox` | A restyled Basic `TextField` |
| `Shadow` | What floats casts it outside itself; `depth` is one of `Theme.shadow`'s, `above` and `below` cut it next to another panel |
| `PanelMotion` | A panel's entrance, exit and turning back, and its blur `area` (`visibleArea`: cut to its window) ([§6.9](visual-coherence.md#69-panels-panelmotion-shadow-dialog-d14d18)) |
| `Dialog` | A window's dialog: title, text, content, `buttons` and `chosen(value)` |

A control goes into `Vela.Controls` only when two programs use it; otherwise it stays with its program, built on Templates and `Fill`: Files' `ToolButton`, the shell's `CheckBox` and `MenuTextBox` (a `TextBox` with the right-click menu of `TextMenu`), Settings' `RadioButton`. Every shell panel is a `PanelMotion` holding its `Shadow` and its panel, in a window that leaves the shadow's `reach` around it; the shell's `ShadowRoom` makes a press in that room a press outside. A state color (hover, press, checked, selected) belongs to a `Fill`, and every duration is a `Theme` token: `scripts/check-qml-motion.py` (ctest `qml-motion`) fails on what doesn't, and a line that differs on purpose says why with a `// motion:` comment. A recycled delegate (`reuseItems`) sets its `Fill`'s `animated` to false when pooled and back to true when reused. The pieces are tested with Qt Quick Test (`common/tests`, ctest `controls` and `controls-no-effects`); the programs' own controls with the same runner (`shell/tests`, `settings/tests`) and Files' views with their own (`explorer/tests`, ctest `files-views`).

## Debugging rendering

`SIGUSR1` forces a full redraw for damage-tracking diagnostics. The real
compositor is named `vela-compositor`; its session supervisor is named
`vela-supervise`. When debugging a running compositor, target its **exact
PID**, not a broad `pkill` that could reach other sessions. Prefer a nested
session for potentially disruptive compositor tests.

Several environment variables are intended specifically for renderer diagnosis. The most useful ones are:

```sh
VELA_VULKAN_VALIDATION=1
VELA_DEBUG_SCANOUT=1
VELA_DEBUG_DAMAGE=1
VELA_DEBUG_SYNC=1
VELA_DEBUG_LINEAR=1
VELA_STATS=1
```

Their exact behavior is documented in [configuration.md](configuration.md).

For Vulkan work, enabling `VELA_VULKAN_VALIDATION=1` during development is strongly recommended.

## Tests

Run the complete suite with:

```sh
ctest --test-dir build --output-on-failure
```

The GPU-dependent functional and sharpness suites are part of CTest.
Hosted runners skip them for lack of a usable DRM/Vulkan device; the
[GPU CI policy](ci-quality.md#gpu-ci-policy) defines required real-hardware
testing and per-release verification.

See [testing.md](testing.md) for individual suites, GPU requirements and the headless harness.

## Contributing

Vela is still moving quickly. Before a large architectural change, opening an issue first is preferred so the direction can be discussed before substantial code is written.

Code, logs, messages and interface text are in English; code comments are still in Italian. Commits should be signed off with:

```sh
git commit -s
```

Contributions are accepted under GPL-3.0-or-later and the Developer Certificate of Origin.
