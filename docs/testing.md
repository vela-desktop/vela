# Testing Vela

Vela's test strategy covers pure compositor logic, desktop components, full headless sessions and pixel-level rendering correctness.

The goal is not only to test individual functions, but to verify properties that are visible to the user: frame timing, exact geometry at fractional scales, window state transitions, output hotplug, lock behavior, crash recovery and copy safety.

## Run everything

After building with `VELA_BUILD_TOOLS=ON` (the default):

```sh
ctest --test-dir build --output-on-failure
```

CTest includes both CPU-only and GPU-dependent suites.
The `gpu` label groups `functional`, `sharpness` and `sharpness-cpu`; the latter
uses CPU fence synchronization while rendering still requires a GPU.

```sh
ctest --test-dir build -LE gpu --output-on-failure # CPU-only suites
ctest --test-dir build -L gpu --output-on-failure  # GPU suites
```

## Test suites

| Suite | What it covers | GPU required |
|---|---|---:|
| **Compositor / GoogleTest** | FrameClock, vblank grid, late latching, adaptive margin, learned host latency, animation curves, output scale selection, pixel geometry, night light, color filters, sunrise/sunset, `vela.conf` parsing and `outputs.conf` persistence. | No |
| **Files** (`files-copies`, `files-views`) | Copy/move behavior, atomic replacement across file/directory types, failed or unsupported exchanges, processes killed immediately before/after exchange, removal errors, and a simulated full disk with `RLIMIT_FSIZE`. Under Qt Quick Test, on a real `FolderModel` over 300 files: the Details and icon views' recycled delegates take their new file's state without a fade while scrolling, and the command bar's `ToolButton` (colors, `checked`, keys, focus, tooltip) ([visual coherence](visual-coherence.md#68-controls-qt-behaves-vela-looks-d12)). | No |
| **Shell** (`shell-*`) | `.desktop` `Exec=` parsing, default-app resolution, volume steps, the asynchronous network model with slow, failed or missing services, `Style` following `animation-effects` and `animation-duration` in `vela.conf`, and switches backed by services (Bluetooth in quick settings, Settings' home and Bluetooth page; Wi-Fi; airplane mode) moving at once and turning back when a fake BlueZ or NetworkManager on a private bus refuses or never does it (`shell-requested-switches`). | No |
| **Vela.Controls** (`controls`, `controls-no-effects`) | Qt Quick Test, offscreen, at three times Vela's durations, then with animation effects off: `Button`, `Switch` and `Slider` reach the color of each state combination, fade and turn back halfway from where they are, take the focus from Tab and not from a click, activate from Space and Enter and set their accessible role and state; a binding on `checked` or `value` survives input; the switch's knob follows a drag and slides only on its own (jumps with effects off); the slider's value is never animated and it reports when the user is done; `PanelMotion` enters, leaves, turns back when reopened without `closed()`, takes no input while leaving (mouse and keys), moves only with effects on, blurs its travel only while moving (cut to its window) and closes at once when asked; `Shadow`'s reach for each depth and its cut above and below; `ListItem`'s colors, keys and ring, `Icon`'s size and source, `Chevron`'s two shapes; `Dialog`'s buttons, Esc, Enter, one choice per opening and fade-out ([visual coherence](visual-coherence.md#68-controls-qt-behaves-vela-looks-d12)). The same runner takes the programs' own controls: the shell's `CheckBox` (`shell-controls`, `shell-controls-no-effects`) and Settings' `RadioButton` (`settings-controls`, `settings-controls-no-effects`). | No |
| **Settings** (`settings-displays`) | Native Wayland inventory and hotplug, modes, scale, rotation, confirmation-only persistence, explicit revert, compositor deadlines with the GUI thread blocked, client disconnection and SIGKILL, competing clients, failed commits and hotplug during preview. Uses real wlroots headless outputs and Vela’s output manager/preview; rendering and window rearrangement are stubbed. | No |
| **Vela Report** (`reporting`, `reporting-gui`) | Guided CLI without a display, shared Qt worker and ZIP export, explicit collection boundaries, redaction, review exclusion, process identity, resource recording, bounded commands and archive failures. | No |
| **Package updater** (`packaging-update`) | Public HTTPS defaults in both the updater and PKGBUILD, first run and cached fetches, migration of saved SSH URLs and old mirrors, local sources, branch/commit selection, exact CI artifacts, download fallback and failed fetch/build/install handling. Uses real Git repositories with simulated package tools. | No |
| **Test infrastructure** (`functional-harness`) | Unexpected compositor exits, explicit intentional failure status, failed startup cleanup, reaping clients, all five PNG filters with known pixels, and exact bounds of changed pixels. | No |
| **Scripts and documentation** (`script-tests`, `documentation`, `shared-values`, `qml-motion`) | Distribution metrics, API failures without partial writes, offline Markdown links/anchors and build dependency lists; the motion durations and curve written in both `motion.h` and `Theme.qml` agreeing ([visual coherence](visual-coherence.md#64-one-duration-scale-written-twice-and-checked-d10)); QML durations that aren't `Theme` tokens and state colors or backgrounds' opacities that don't fade, failing ([§6.11](visual-coherence.md#611-keeping-it-so)). | No |
| **Functional** (`functional`) | Opening and manipulating windows (Wayland and X11), menus (popups and X11 override-redirect menus) kept where they belong, resizing from the invisible borders, keyboard Move/Resize, the window menu, drag-to-edge snap with Snap Assist, snap layouts and snap groups, snap, maximize/restore, minimize/Alt+Tab, virtual desktops, Vela's title bar (text, buttons, double click, the shell's Mica colors and accent, the buttons' fading hover, no seam with Settings' content), every window animation from every trigger (with the app's images in its snapshots), the shell's panels entering animated (Start, Quick Settings, Notification Center, Task View, captured at 300% duration), dialogs and panels leaving animated and menus and Task View at once, the exit, shadow room and blur during the slide of Start, Quick Settings, Notification Center and clipboard, Quick Settings' page heights, animation durations following `animation-duration`, shell layers (reserved space, live blur, Vela's acrylic only on `vela-` surfaces and a plain blur for other clients), partial redraws identical to full redraws, output hotplug, per-output scale and position set like Settings does, accessibility quick settings (night light and its schedule, color filters, magnifier, sticky keys), Super alone opening Start, the media and volume keys (held volume keys repeat), the mouse's side buttons as Back and Forward in Vela's apps (File Explorer, also with the focus elsewhere or on an inactive window), lock/unlock, screen power, compositor crash recovery, a shell that crashes (restarted, then given up) or exits cleanly (left alone), and an app whose GPU finishes 150 ms late (explicit and implicit sync) without a missed vblank, and the polkit agent (below). | Yes |
| **Audio** (`audio-*`) | The private PipeWire and WirePlumber the audio tests run on ([audio](audio.md#11-tests)): fake devices, defaults, streams following the default output, recording, levels, restarts and isolation from the user's audio. On it, the `vela-audio` library: the model, changes from outside, the volume's grid and 100% cap, balance, values shown at once and the server winning, microphone actions shown only once confirmed, apps grouped by program with their volume, mute, output and input, devices appearing, PipeWire and WirePlumber restarts; a Bluetooth headset's battery from a fake BlueZ. The shell's sound keys on it: volume keys, the microphone key whose indicator waits for the server, the wheel on a taskbar button. | No |
| **Polkit agent / GoogleTest** (`polkit.*`) | The agent's queue, cancellations by polkit and by the user, a crashed dialog counting as "No", retries after a wrong password, sessions that fail by themselves, identity order and choice, the dialog protocol. | No |
| **Sharpness** (`sharpness`) | Pixel-level checks that windows reach the expected physical pixels across fractional scales and common window states. | Yes |

The compositor currently contains 83 GoogleTest cases and the polkit agent 23;
the functional harness runs 103 end-to-end scenarios when the Qt shell is built.
A complete build registers 143 CTest entries; some entries contain multiple
scenarios. Every scenario requires the expected compositor exit status, normally
0, even if it exited before cleanup. The deliberate three-crash supervisor test
explicitly requires status 1. Unexpected crashes cannot become passing tests.

CPU tests of the harness and scripts are included in CTest even on GPU-less CI.
PNG decoding is checked with fixed encoded rows and independently specified
pixels, including Paeth tie breaking and byte wraparound.

## Run a subset

Compositor unit tests:

```sh
ctest --test-dir build -R '^compositor\.' --output-on-failure
```

Files and shell tests:

```sh
ctest --test-dir build -R 'files-copies|shell-' --output-on-failure
```

For replacements between different item types, Explorer stages the complete copy
and exchanges it with the destination using Linux `renameat2(RENAME_EXCHANGE)`.
If the exchange fails (including unsupported kernels/filesystems), the old
destination stays in place; there is no delete-then-rename fallback. After a
successful exchange, the old item is removed from the hidden staging path. If
that removal fails, the job reports the remaining path and a move retains its
source. A process killed before that cleanup may leave a hidden
`.name.vela-copy-XXXXXX` item containing the old destination. These interruption
tests kill a separate process at the syscall boundary; they do not simulate
power loss. Directory-to-directory replacements still merge file by file.

The `shell-service-models` suite starts fake `nmcli` clients, with a private
D-Bus bus (`dbus-daemon` is required). It verifies that refresh calls return
while the service is waiting, GUI-thread timers continue firing, cached
networks survive errors, timeouts and slow Wi-Fi scans, saved networks reuse
their profile with friendly errors, a missing program completes and recovers,
and destroying a model terminates its clients without waiting. No real network
service is used; sound has its own tests, below.

Audio tests:

```sh
ctest --test-dir build -R '^audio-' --output-on-failure
```

They start their own PipeWire and WirePlumber (`pipewire` and `wireplumber`
installed, nothing else) in a temporary directory, with fake devices made of
null sinks. The user's audio, WirePlumber state and D-Bus services are never
touched; without the programs the tests are reported as skipped. To look
around inside the fixture by hand:

```sh
VELA_BUILD=build python3 tests/audio/fixture.py -- wpctl status
```

Display protocol and compositor-owned recovery:

```sh
ctest --test-dir build -R '^settings-displays$' --output-on-failure
```

The display fixture uses isolated Wayland sockets and configuration directories,
with a shortened compositor deadline. Its killed process runs the same native
`Displays` backend as Settings. These tests exercise the real protocol and output
commits without a GPU; the functional suite still supplies renderer and window
placement coverage on real hardware.

Size-only changes through the native Settings backend preserve the selected
display's facing edge, gap and transverse alignment with a neighboring display.
The fixture covers monitors above, below, left and right, fractional scaling,
explicit positions, preview rollback and insufficient space between two other
outputs. An upper 1080p screen rotated to portrait must move from `y=-1080` to
`y=-1920`, rather than overlap the screen below it.

The GPU shell test repeats that reported layout with the real Qt desktop:
2560x1440 at 125% below 1920x1080 at 100%. It requests only the transform through
the actual `Displays` client, checks both wallpaper and taskbar rectangles, and
compares the main screen's pixels across 90°, 270° and landscape transitions.
Direct `vela-randr --pos` tests alone cannot verify Settings chooses the right
position. Layer entries in diagnostic `state` include their live logical bounds.

Diagnostic reporter and graphical smoke check:

```sh
ctest --test-dir build -R '^reporting' --output-on-failure
```

Functional session tests, one ctest entry per module of `tests/functional`
(`test_shell.py` is `functional-shell`):

```sh
ctest --test-dir build -L '^functional$' --output-on-failure
ctest --test-dir build -R '^functional-shell$' --output-on-failure
```

Package and updater contracts (also runnable without building Vela):

```sh
python3 -m unittest discover -s packaging/tests -v
ctest --test-dir build -R '^packaging-update$' --output-on-failure
```

These tests require Python 3.10+, Git, Bash and a POSIX shell. Each scenario uses
temporary repositories and an isolated home/config/cache. Git's global and
system configuration is disabled; only file transport is allowed. The test
adapter records the requested clone/fetch URL, rejects SSH and unexpected
repositories, and maps the expected public HTTPS URL to a real local Git
repository. Package builds, authentication, artifact downloads and installation
are simulated, so the suite needs no network, GPU, Arch installation, SSH keys,
GitHub login or administrator privileges.

The previous suite always passed `--source` with a local repository; package CI
also set `VELA_GIT_URL` to a local checkout. Both bypassed the public defaults.
The first-run and PKGBUILD default tests now exercise those paths explicitly.

Sharpness only:

```sh
ctest --test-dir build -R '^sharpness$' --output-on-failure
```

For direct invocation, the functional entry point is:

```sh
python3 tests/functional/run.py
python3 tests/functional/run.py --sharpness
```

The sharpness check additionally needs NumPy and Pillow available to `python3`.

## GPU requirements

`tests/functional/test_output_rotation.py` exercises mixed landscape/portrait
outputs, all eight Wayland transforms on an already maximized window, an
output placed at negative coordinates (the compositor then moves the layout's
origin to 0,0) at 150% scale, and four-output layouts with explicit
transform assertions. Every interior pixel of the maximized `vela-pattern`
window is compared with its independently generated coordinate pattern;
nonblank pixels or equality with a previous capture are insufficient. Stability
captures must span distinct presented frames. A portrait crop also verifies
that screen-dump regions use upright image coordinates.
Software-cursor-only damage is checked after rotating the output, with a shader
color filter forcing cursor composition: old cursor pixels must be restored
and the new pointer drawn without relying on capture's PAINT_CURSORS option.
The output's damage event arrives in upright output coordinates (wlroots'
header says buffer-local, but its cursors emit their box as is) and joins the
scene damage unchanged; the frame converts it to the buffer once.
The four-output case also renders three simultaneous maximized coordinate
patterns and checks their pixels on the individual outputs.

`vela-screen-dump` normalizes image dimensions and samples pixels with the
inverse of the protocol's buffer-to-output transform before PNG encoding.
`VELA_SCREEN_DUMP_CURSOR=1` includes the pointer for coordinate checks.
CPU capture tests cover all eight
transforms on a rectangular buffer with padded rows, including bounds and
invalid-transform rejection. These GPU tests use real hardware with headless
outputs; physical-monitor rotation and driver coverage still need separate
validation.

The functional and sharpness suites use the real Vulkan renderer. They intentionally do not fall back to software rendering.

They require:

- a usable `/dev/dri/renderD*` device;
- a GPU/driver exposing Vulkan 1.4 as required by Vela;
- NumPy and Pillow for the pixel sharpness suite.

If the harness cannot find a usable GPU, it exits with status 77. CTest marks that as **skipped**, not failed.

### GPU CI hardware

The complete GPU-dependent suite runs on two machines, through the
[GPU CI workflows](ci-quality.md#manual-real-gpu-ci-self-hosted-amd):

| GPU | Driver | OS |
|---|---|---|
| AMD Radeon RX 9070 XT | Mesa/RADV | CachyOS |
| NVIDIA GeForce RTX 4070 Laptop GPU | NVIDIA proprietary | Arch/CachyOS |

The workflows are started by hand, but they run before every merge into
`main` and before every release, and a merge needs both of them to pass.

This gives the renderer real-hardware coverage on both Vulkan drivers. It is not the same as a broad hardware matrix: Intel paths currently receive no repeated real-hardware testing, and both runners use the headless backend, not DRM/KMS ([hardware coverage](hardware.md)).

## Headless functional harness

`scripts/run-headless.sh` starts a real Vela session on wlroots' headless backend. No Vela window appears on the current desktop, but apps can be opened, moved, snapped and captured exactly as in a visible session.

```sh
sh scripts/run-headless.sh &
export WAYLAND_DISPLAY=wayland-1
```

Use the actual display name printed in the log.

The test tools can then drive and inspect it:

```sh
build/tools/vela-input key super+Left
build/tools/vela-windows list
build/tools/vela-screen-dump screen.png
build/tools/vela-randr --output HEADLESS-1 --scale 1.5
```

The command socket is:

```text
$XDG_RUNTIME_DIR/vela-$WAYLAND_DISPLAY.sock
```

Important test commands include:

```text
state
test-output add 2560x1440
test-output remove HEADLESS-2
test-power off
test-power on
reload-config
```

`state` returns JSON describing the compositor-visible state. Functional scenarios assert against that state instead of relying on arbitrary sleeps.

A simplified scenario looks like:

```python
with Session(scale=1.25) as vela:
    window = vela.open_window()
    vela.keys("super+Left")
    vela.wait_for(lambda s: s.window(window)["snap"] == [0, 0, 6, 12])
```

## Polkit agent

`tests/functional/test_polkit.py` never talks to the real polkitd or PAM: a wrong password sent to PAM can lock the account (`pam_faillock`).

- **The dialog** (`vela-polkit-prompt`) is started by the test, which plays the agent over its stdin/stdout ([protocol](polkit-agent.md#5-agent--prompt-protocol)). Covered: veil and dialog on the right outputs, the keyboard kept by the dialog while windows open, shortcuts switched off, clicks outside going nowhere, answers, "try again", Esc, cancellation by the agent, a dead agent, unavailable authentication, choosing another identity, a keyboard Move ended by the dialog.
- **The agent** (`vela-polkit-agent`) is started by the compositor (`VELA_POLKIT_AGENT`) and registers with `fake_polkitd.py`, a fake polkitd on a private D-Bus bus passed as `DBUS_SYSTEM_BUS_ADDRESS`. `VELA_POLKIT_TEST_PASSWORD` replaces PAM with a session that accepts one password; the agent refuses that mode unless the authority reports itself as the fake one. Covered: registration for the session in Vela's language, authorizing after a wrong password, "No", a request withdrawn by polkit, a crashed dialog counting as "No", queued requests, restart after a crash, no restart when another agent owns the session, test mode refused against another authority.

The agent tests need `dbus-python` and `dbus-daemon`; without them they are skipped.
Strict self-hosted GPU CI treats these skips as failures. On the Arch/CachyOS AMD
runner, install the test dependencies **on the host** (not in the workflow):

```sh
sudo pacman -S --needed python-dbus dbus
/usr/bin/python3 -c 'import dbus; print("D-Bus Python bindings OK")'
command -v dbus-daemon
```

The AMD workflow intentionally pins `-DPython3_EXECUTABLE=/usr/bin/python3`
for CTest and verifies D-Bus on that exact interpreter before building. This
prevents CMake from preferring another `~/.local/bin/python3.12` which cannot
import the system-installed D-Bus bindings (as happened in AMD run 40130281376).
The interactive `python3` on `PATH` may be different from CMake's selection.
For CTest sharpness, `tests/functional/run.py` passes its interpreter to
`scripts/test-sharpness.sh` through `VELA_TEST_PYTHON`, ensuring all NumPy/Pillow
checks and pixel comparisons use that same interpreter. When run standalone,
the shell script still defaults to `python3` from `PATH`.

`state` also lists the layer-shell surfaces (`layers`: namespace, layer, keyboard interactivity, output) and the one with the keyboard (`focusedLayer`).

## Sharpness testing

The sharpness suite launches the compositor headlessly at several output scales and compares captured pixels against deterministic expectations.

The standalone script can also be run directly:

```sh
sh scripts/test-sharpness.sh
```

By default it checks multiple fractional scales. Explicit scales may be passed as arguments:

```sh
sh scripts/test-sharpness.sh 1 1.25 1.5 1.75 2
```

The test exercises normal, snapped, maximized and restored window states so fractional-scale correctness is checked across window-management transitions rather than only in a static scene.

## Messages to the shell

The compositor tells the Qt shell what to show (Start, Alt+Tab, the window menu,
accessibility state) with one line per message on
`$XDG_RUNTIME_DIR/vela-shell-$WAYLAND_DISPLAY.sock`. In the harness,
`vela.fake_shell()` listens on that socket and collects the lines, so scenarios can
assert on them without the real shell:

```python
shell = vela.fake_shell()
vela.keys("super")
shell.wait_for("toggle-start")
```

## Slow apps

`tests/functional/test_ready.py` checks that an app whose GPU is late never makes the
screen wait (see [renderer.md](renderer.md) §7.3). `tools/vela-slowgpu` commits each frame
while a Vulkan compute job, on a queue separate from the graphics one, still runs for about
150 ms; its fence becomes the buffer's acquire point (`linux-drm-syncobj-v1`) or, with
`--implicit`, the dmabuf's write fence. While the cursor moves for a second, the test reads
from `state` the missed vblanks, the frames shown and the commits held back:

| Scenario | Expectation |
|---|---|
| explicit sync | at most 2 missed vblanks, commits held back |
| implicit sync | at most 2 missed vblanks, commits held back |
| CPU synchronization (`VELA_SYNC_FILE=0`) | implicit producer fences still respected, at most 2 missed vblanks |
| `VELA_READY_WAIT=0` | at least 20 missed vblanks: proves the test really detects a stall |

`tests/functional/test_capture.py` captures during four maximize/restore cycles
at 2560x1440@180 Hz and fails on any capture error. When the allocated readback
staging memory is cached, it also requires fewer missed vblanks than captures.
GPUs that fall back to uncached staging must still capture successfully, but
cannot be expected to meet this performance target. The compositor logs
`Vulkan readback staging: uncached fallback` when this happens; historically,
each uncached capture cost about three missed vblanks on the tested AMD GPU.

`tests/functional/test_resources.py` runs both GPU and CPU synchronization
under a 1024-descriptor limit, renders 1200 frames after warm-up, and checks
the compositor's actual FD and sync_file counts for bounded growth. Its
supervisor test stops the compositor with SIGSTOP and verifies that resource
records continue in the session log before resuming it. The CPU-only tests
also check interrupted fence waits, closing ownership, 2048 wait/close cycles,
driver policy and read-only process sampling. Hosted CI cannot establish
NVIDIA driver correctness; the GPU scenarios must also run on affected hardware.

Reporter fixtures cover complete logs above the former 2 MiB cap, preserving
startup and final errors in oversized logs, refreshing evidence after live
recording, rotation, historical descriptor records and matching retained logs
to their incident boot. An unknown boot must never become current-boot evidence.

`tests/functional/test_shell.py` runs the actual Qt shell with a private D-Bus
session and isolated configuration, using threaded OpenGL rendering, CPU
compositor synchronization and a 100 Hz output. It checks repeated Super
toggles, reopening during the close animation, and the Wayland trace of each
native surface lifetime: configure and ACK precede the first buffer, the hidden
menu releases its native surface, and blur binds to the new surface on reopen.
Run, Quick Settings, Notification Center and clipboard also reopen after
native surface destruction. Quick Settings, Notification Center and clipboard
repeat Start's toggles; all four turn back when reopened during their exit,
with the window never unmapped, and close on a click in their shadow's room
but not on their own background. Between pages Quick Settings resizes its
window exactly twice while its panel animates (docs/visual-coherence.md §6.9,
§6.10). Run, Properties and the clipboard leave in `normal` (600 ms at 300%),
the desktop menu and Task View at once. `PanelBlur`, on a private striped
wallpaper, finds blur behind every row of Quick Settings, Start and the
clipboard in every captured frame of their entrance, also below where they
rest, and behind every column of the Notification Center sliding in from the
right; at rest the blur region is the panels only and their shadow isn't
blurred. A fake shell or `vela-panel` cannot exercise this Qt lifecycle.
Selection and snipping tests cover all eight transforms, 100%, 125% and 150%
scales, and outputs above, below, left and right in eight representative cases.
Additional snips cover odd 1367x769 dimensions at 125% and 4K at 200%.
A held virtual pointer grows
and shrinks the desktop band, then releases it; only the expected upright
rectangle may change, with exact unzoomed bounds. The 2x magnifier allows only
the three-pixel Catmull-Rom footprint at its top/left edges. A fixed private SVG
gradient avoids dependence on the installed wallpaper or theme image.
Both Print and Super+Shift+S must map overlays on every
output and save exactly the selected pixels to an isolated Pictures directory.
Odd logical selection endpoints at 125% and 150% scale additionally verify that
the crop rounds its two physical edges independently, rather than rounding
origin and width separately.
Qt PNG filters are decoded before comparison. Captures with the wlroots cursor
independently anchor image coordinates to the pointer position and shape, so
opposite errors in capture and scene rotation cannot cancel each other. A zoomed
portrait case checks that the magnifier preserves cursor/selection alignment.
The default Qt sessions use CPU synchronization; set `VELA_QT_SYNC_FILE=1` to
repeat them with GPU fence synchronization (the renderer remains Vulkan).
Coverage is sampled rather than an exhaustive product of every transform,
scale, resolution, layout, refresh rate and client buffer transform. These tests
do not measure sustained frame-drop rates or exercise DRM hardware cursor
planes, HDR/color-management combinations or every Intel/NVIDIA driver. The
physical-monitor smoke test complements the headless GPU scenarios.
The desktop tests assert the selection band's rendering; icon selection/drag
state on a portrait primary output still needs a dedicated UI scenario. Snipping
currently verifies rectangular crops; window/full-screen modes, cancellation,
pending-capture timeouts and screen removal during a snip need separate cases.
These tests do not establish that a driver-specific protocol error is fixed
on NVIDIA; they enforce the observable lifecycle and animation contracts.
Reporter fixtures separately check protocol errors and shell restarts without
descriptor exhaustion, including removing derived summaries during review.

`sharpness-cpu` repeats the pixel comparison at all five scales with
`VELA_SYNC_FILE=0`, so CPU compatibility must preserve exactly the same pixels.
Both sharpness suites honor `VELA_BUILD`, including build paths with spaces,
and fail immediately if an executable is missing.

## Vulkan validation and renderer diagnosis

For renderer work, run relevant scenarios with Vulkan validation enabled:

```sh
VELA_VULKAN_VALIDATION=1 ctest --test-dir build -R 'functional|sharpness' --output-on-failure
```

For functional sessions, the harness also requires the requested validation
layers to be enabled and fails on logged Vulkan validation errors, including
errors emitted during compositor shutdown.

Additional rendering switches such as `VELA_DEBUG_DAMAGE`, `VELA_DEBUG_SYNC`, `VELA_DEBUG_LINEAR` and `VELA_DEBUG_SCANOUT` are documented in [configuration.md](configuration.md).

## Continuous integration

GitHub Actions builds Vela and runs every test that does not require a real DRM/Vulkan device. The GPU-dependent suites remain registered in CTest but are reported as skipped on hosted runners.

The fast prerequisite job also runs the reporter evidence contracts before
building the desktop, including historical boot selection and log retention.

The Build workflow runs the package/updater contracts in a separate, fast job
before compiling Vela. The suite also runs through CTest in the Arch build, and
unittest discovery automatically includes new packaging test files.

That means CI protects compilation and CPU-only logic on every push, while the primary development workstation currently provides the repeated real-GPU validation.

A future hardware CI matrix would broaden driver and GPU coverage rather than introduce GPU testing from scratch.
