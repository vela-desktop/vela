# Developing Vela

This document covers the day-to-day development workflow for Vela: building, running nested or headless sessions, the repository layout, and the tools used to inspect a live compositor.

For renderer design and frame scheduling, see [renderer.md](renderer.md). For the complete test setup, see [testing.md](testing.md). Runtime overrides and debug switches are documented in [configuration.md](configuration.md).

## Build

Vela requires CMake 3.22 or newer, Ninja, a C17 and C++20 compiler, wlroots 0.20, Qt 6.7 or newer with its Linguist tools, LayerShellQt, Vulkan headers and loader, `glslc`, GBM, libdrm, FreeType, HarfBuzz, Fontconfig and PAM. librsvg is optional (app icons in the title bar). The development tools and tests also need zlib, GoogleTest and Python 3. Distribution package lists are in the [README](../README.md#build-from-source).

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
| `vela-pattern` | Produce deterministic visual test content (`--decorated` asks for Vela's title bar, `--popup X,Y` opens a 200×150 magenta menu anchored at that point of the window). |
| `vela-panel` | Fake shell pieces on wlr-layer-shell: `wallpaper`, and `taskbar [--blur]` with a reserved area and background blur. |
| `vela-x11` | An X11 app (through Xwayland) for the X11 tests: an orange 300×200 window with class `vela.x11` that draws only on Expose, like most apps; `--menu X,Y` adds an override-redirect menu. |
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
```

`state` returns the compositor's visible state as JSON and is the primary assertion surface used by the functional test harness.

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
common/             Vela.Controls: theme, controls and blur shared by the Qt Quick apps
session/            login entry, systemd target and portal integration
packaging/arch/     PKGBUILD and vela-update
report/             vela-report: shared Python core/CLI and standalone Qt Widgets GUI
tools/              development and test clients
tests/functional/   headless functional scenarios
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

## Debugging rendering

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

The GPU-dependent functional and sharpness suites are part of CTest as well. They are run regularly on Vela's primary development machine with an AMD Radeon RX 9070 XT and Mesa/RADV, where they currently pass in full. GitHub-hosted CI skips those suites because it does not expose a usable DRM/Vulkan device.

See [testing.md](testing.md) for individual suites, GPU requirements and the headless harness.

## Contributing

Vela is still moving quickly. Before a large architectural change, opening an issue first is preferred so the direction can be discussed before substantial code is written.

Code, logs, messages and interface text are in English; code comments are still in Italian. Commits should be signed off with:

```sh
git commit -s
```

Contributions are accepted under GPL-3.0-or-later and the Developer Certificate of Origin.
