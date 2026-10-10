# Testing Vela

Vela's test strategy covers pure compositor logic, desktop components, full headless sessions and pixel-level rendering correctness.

The goal is not only to test individual functions, but to verify properties that are visible to the user: frame timing, exact geometry at fractional scales, window state transitions, output hotplug, lock behavior, crash recovery and copy safety.

## Run everything

After building with `VELA_BUILD_TOOLS=ON` (the default):

```sh
ctest --test-dir build --output-on-failure
```

CTest includes both CPU-only and GPU-dependent suites.

## Test suites

| Suite | What it covers | GPU required |
|---|---|---:|
| **Compositor / GoogleTest** | FrameClock, vblank grid, late latching, adaptive margin, learned host latency, animation curves, output scale selection, pixel geometry, night light, color filters, sunrise/sunset, `vela.conf` parsing and `outputs.conf` persistence. | No |
| **Files** (`files-copies`) | Copy/move behavior, atomic replacement across file/directory types, failed or unsupported exchanges, processes killed immediately before/after exchange, removal errors, and a simulated full disk with `RLIMIT_FSIZE`. | No |
| **Shell** (`shell-*`) | `.desktop` `Exec=` parsing, default-app resolution, volume steps, and asynchronous audio/network models with slow, failed or missing services. | No |
| **Vela Report** (`reporting`, `reporting-gui`) | Guided CLI without a display, shared Qt worker and ZIP export, explicit collection boundaries, redaction, review exclusion, process identity, resource recording, bounded commands and archive failures. | No |
| **Package updater** (`packaging-update`) | Public HTTPS defaults in both the updater and PKGBUILD, first run and cached fetches, migration of saved SSH URLs and old mirrors, local sources, branch/commit selection, exact CI artifacts, download fallback and failed fetch/build/install handling. Uses real Git repositories with simulated package tools. | No |
| **Functional** (`functional`) | Opening and manipulating windows (Wayland and X11), menus (popups and X11 override-redirect menus) kept where they belong, resizing from the invisible borders, keyboard Move/Resize, the window menu, drag-to-edge snap with Snap Assist, snap layouts and snap groups, snap, maximize/restore, minimize/Alt+Tab, virtual desktops, Vela's title bar (text, buttons, double click), shell layers (reserved space, live blur), partial redraws identical to full redraws, output hotplug, per-output scale and position set like Settings does, accessibility quick settings (night light and its schedule, color filters, magnifier, sticky keys), Super alone opening Start, the media and volume keys (held volume keys repeat), the mouse's side buttons as Back and Forward in Vela's apps (File Explorer, also with the focus elsewhere or on an inactive window), lock/unlock, screen power, compositor crash recovery, a shell that crashes (restarted, then given up) or exits cleanly (left alone), and an app whose GPU finishes 150 ms late (explicit and implicit sync) without a missed vblank, and the polkit agent (below). | Yes |
| **Polkit agent / GoogleTest** (`polkit.*`) | The agent's queue, cancellations by polkit and by the user, a crashed dialog counting as "No", retries after a wrong password, sessions that fail by themselves, identity order and choice, the dialog protocol. | No |
| **Sharpness** (`sharpness`) | Pixel-level checks that windows reach the expected physical pixels across fractional scales and common window states. | Yes |

The compositor currently contains 59 GoogleTest cases and the polkit agent 23; the functional harness runs 87 end-to-end scenarios when the Qt shell is built. Every scenario also checks that the compositor exits with status 0 when asked to stop: a crash on the way out would look like a real one to the supervisor.

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

The `shell-service-models` suite starts fake `pactl`, `pw-metadata` and `nmcli`
clients and a delayed BlueZ service on a private D-Bus bus (`dbus-daemon` is
required). It verifies that refresh calls return while services are waiting,
GUI-thread timers continue firing, cached models survive errors and timeouts,
refresh bursts coalesce, old reads cannot undo newer volume commands, the first
taskbar wheel step waits for streams, and destroying a model terminates its
clients without waiting. No real audio, network or Bluetooth service is used.

Diagnostic reporter and graphical smoke check:

```sh
ctest --test-dir build -R '^reporting' --output-on-failure
```

Functional session tests:

```sh
ctest --test-dir build -R '^functional$' --output-on-failure
```

Package and updater contracts (also runnable without building Vela):

```sh
python3 -m unittest discover -s packaging/arch/tests -v
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

The functional and sharpness suites use the real Vulkan renderer. They intentionally do not fall back to software rendering.

They require:

- a usable `/dev/dri/renderD*` device;
- a GPU/driver exposing Vulkan 1.4 as required by Vela;
- NumPy and Pillow for the pixel sharpness suite.

If the harness cannot find a usable GPU, it exits with status 77. CTest marks that as **skipped**, not failed.

### Primary development hardware

The complete GPU-dependent suite is run regularly during development on:

- **GPU:** AMD Radeon RX 9070 XT
- **Driver:** Mesa/RADV
- **OS:** CachyOS

The functional and sharpness suites currently pass in full on that machine.

This gives the renderer regular real-hardware coverage during development. It is not the same as a broad hardware matrix: Intel and NVIDIA paths currently receive less repeated real-hardware testing than AMD/RADV.

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
build/tools/vela-shot screen.png
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
Run, Quick Settings and Notification Center also reopen after native surface
destruction. A fake shell or `vela-panel` cannot exercise this Qt lifecycle.
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
