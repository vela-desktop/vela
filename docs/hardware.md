# Hardware coverage and alpha limitations

Vela needs a GPU and driver exposing **Vulkan 1.4** with the dmabuf/DRM capabilities described in [renderer.md](renderer.md#71-minimum-requirement). A successful build does not guarantee that a particular GPU, driver, compositor nesting setup or display configuration works.

## Evidence, not a compatibility guarantee

Two machines run the GPU-labeled suites (functional, sharpness and
sharpness-cpu): an **AMD Radeon RX 9070 XT** and an
**NVIDIA GeForce RTX 4070 Laptop GPU**. See the
[GPU CI policy](ci-quality.md#gpu-ci-policy) for mandatory coverage,
candidate revision checks and release rules.

| Environment | What has been established | What remains uncertain |
|---|---|---|
| AMD Radeon RX 9070 XT + Mesa/RADV, CachyOS | Primary development machine and the AMD GPU CI runner: the GPU suites run on it for relevant GPU changes and releases, and it is used daily as a DRM session | Not representative of every AMD generation or every multi-display setup |
| NVIDIA GeForce RTX 4070 Laptop GPU + proprietary driver, Arch/CachyOS | The NVIDIA GPU CI runner: the same GPU suites for relevant GPU changes and releases; driver-specific synchronization handling; manual tester reports | The runner's suites are headless, so DRM/KMS, screen sharing and suspend/resume rely on manual reports; known failures are tracked as issues (such as [#132](https://github.com/vela-desktop/vela/issues/132)) |
| Intel + Mesa/ANV | Vulkan support is an intended path | No repeated automated real-hardware matrix is documented |
| NVK | Listed as an intended Vulkan driver option | Compatibility is not demonstrated by the AMD test runs |
| VMs and virtio/Venus | Useful experimental debugging environment | Vulkan version, modifiers, dmabuf import and compositor behavior depend on the VM stack |
| Hybrid and multi-GPU systems | Supported configurations are an architectural goal | Correct operation across GPU boundaries needs dedicated tests |

These are **coverage descriptions**, not a ranking of hardware or a statement that every item was individually tested in this documentation change.

## Before trying Vela

- Retain a working desktop session or TTY as a fallback. Vela is alpha software.
- Confirm Vulkan 1.4, a usable render node and compatible drivers.
- Prefer [running nested](../README.md#try-it-without-logging-out) before using Vela as your login session. Nested mode does not prove physical DRM/scanout works.
- For reporting, note distro, kernel, GPU, Vulkan driver, GPU driver version, display mode/refresh/scale, nested or DRM session and the Vela commit.
- The headless functional and sharpness tests check important invariants but do not replace observing the real monitor and driver.

## Integration services

Different features rely on installed/running system services. On Arch, [vela-git](../packaging/arch/PKGBUILD) installs Vela Core and the standard Vela Desktop integration together. Additional hardware/services remain optional.

| Feature | Typically needed |
|---|---|
| Network / Wi-Fi | NetworkManager |
| Bluetooth | BlueZ, with `bluetooth.service` running ([installation](installation.md#bluetooth)) |
| Audio controls and per-app volumes | PipeWire and WirePlumber (PulseAudio without PipeWire is not supported) |
| X11 apps and older games | Xwayland |
| Removable and unmounted storage | UDisks2 |
| Screen sharing / application portals | `xdg-desktop-portal` and `xdg-desktop-portal-wlr` are installed by default on Arch. `xdg-desktop-portal-gtk` provides generic application portals by default, while wlr handles screenshot and screencast |
| KDE styling and secrets | Appropriate KDE icon/theme/wallet components |

Vela does not install `xdg-desktop-portal-kde` or Plasma Workspace. Konsole and Breeze icons remain default desktop components. BlueZ remains optional for native Vela Bluetooth integration.

The Vela compositor and Vulkan renderer are Vela's own; reusing standard Linux services and selected KDE components does not mean running KDE's compositor or Plasma shell.

## Suggested manual smoke tests

On the affected physical GPU, test login/logout, multiple displays and scaling, Alt+Tab and snapping, locking/unlocking, audio mute and network scanning, screenshots and screen sharing, device hotplug, suspend/resume and recovery from a compositor/shell crash where safe. Record observed pass/fail/untested status per test; do not infer it from a generic CI green check.

### Sound

The automated audio tests run on a private PipeWire whose devices are null sinks ([audio](audio.md#11-tests)): no sound card, no Bluetooth, no hardware mixer. These need a real machine:

- **Volume**: the volume keys and a knob on a real sound card move the hardware mixer (through the device's route); the volume is the same after logging out and in.
- **Headphones**: unplugging hides them and moves the sound to the next output; plugging them back returns the sound to them if they were the default.
- **Sound card**: switching profiles, HDMI/DisplayPort audio, turning a device off and on.
- **Bluetooth headset**: battery, codecs, call mode, devices with few volume steps.
- **Microphone**: the mute key and its indicator; on laptops, its LED. The microphone icon in the tray with real apps (a browser, Discord, OBS), including a paused recording and echo-cancelling or noise filters.
- **Sample rate and format** (Sound › Advanced and a device's properties): on a USB DAC, "Automatic" following the music, a fixed device rate, a sample format, and the format shown live while the music changes; an HDMI receiver and a Bluetooth headset with "Automatic".
- **Restart audio**: sound comes back, and the shell and Settings follow without restarting.
- **Rename**: a device's new name shows at once in Vela, and in other apps (pavucontrol, a browser's device list) after logging out and in or "Restart audio"; going back to the device's own name the same way.

### Bluetooth

The automated Bluetooth tests run the real `vela-bluetooth` against a fake BlueZ ([Bluetooth](bluetooth.md#11-tests)): no radio, no real device. In a Vela session with only `bluez` installed (no BlueDevil or blueman), these need a real machine:

- **Devices**: an LE mouse; a keyboard (passkey or legacy PIN); a phone (numeric comparison, started from Vela and from the phone); a headset (battery in quick settings and in the sound page, codec and profile in Sound).
- **Two at once**: a phone asking to pair while a mouse is pairing.
- **Adapters**: unplugging a USB dongle; two adapters at once, and choosing one in Settings › Bluetooth & devices › Advanced.
- **A laptop with a platform Bluetooth switch** (asus-wmi, hp-wmi...), blocked from Plasma: the switch in Vela shows Off and turning it on brings the radio back.
- **`systemctl restart bluetooth`** with Settings open: the page empties and comes back by itself.
- **Searching, other scan first**: start `bluetoothctl scan on`, then open "Add a device". Vela says another app is searching and does not make the PC discoverable. Close both searches.
- **Searching, Vela first**: open "Add a device" while nothing else scans, then start `bluetoothctl scan on` and close Vela's Add dialog. If BlueZ leaves the PC visible, Vela offers "Stop being visible".
- **Locking** the screen with "Add a device" open: the search and the pairing stop; after unlocking, "Search again".
- **Another desktop** (Plasma) on another console at the same time:
  - pairing from Plasma while Vela is in the background works with Plasma's agent;
  - Plasma's discoverability is not changed by Vela;
  - switching back to Vela gives Vela the agent again.

`vela-bluetooth` writes what it does (adapters, operations, questions, searches, the session) to the session log, `~/.local/state/vela/vela.log` (the previous session's is `vela.log.old`; [reporting](reporting.md) collects it): `grep vela-bluetooth ~/.local/state/vela/vela.log`. `bluetoothctl show` and `bluetoothctl devices` are the reference for what BlueZ itself says.

### Brightness

The automated brightness tests run on fake monitors and a fake backlight ([brightness](brightness.md#12-tests)). Vela speaks DDC/CI itself, on `/dev/i2c-N`, and installs what it needs: a udev rule that gives the session's user the I²C buses of display adapters (`/usr/lib/udev/rules.d/70-vela-i2c.rules`) and `i2c-dev` at boot (`/usr/lib/modules-load.d/vela-i2c.conf`). `ddcutil` is not needed; it is the reference for checking the slider.

- **No slider for an external monitor**: many monitors ship with **DDC/CI turned off** in their own menu (Settings › Display says so). Turning it on and coming back to Settings is enough. Docks and MST hubs often don't pass DDC/CI through.
- **"Vela can't reach this display"**: the udev rule or `i2c-dev` isn't active yet; log out and in after installing, or reboot.
- **NVIDIA's proprietary driver** may need the I²C options that ddcutil documents (its `nvidia-i2c.conf`).
- `vela-brightness --probe` (in `/usr/lib/vela`, or `/usr/libexec/vela` on Fedora) prints each connector, its bus and one read; it only reads. Vela Report includes it under "Display brightness".

To check on a real machine, with `ddcutil getvcp 10` as the reference:

- Every monitor gets a slider or the right reason; a drag ends on the value shown; 0% and back.
- The monitor's own buttons move a slider that is on screen.
- Unplug and replug, idle off and back on, VT switch, suspend and resume with Settings open.
- NVIDIA (the `read()`/`write()` fallback) and Intel; a laptop: the backlight chosen, its minimum, OLED if available; a dock or MST hub.

See [testing.md](testing.md) and [reporting.md](reporting.md) for automated tests and privacy-aware diagnostics.

## GPU resource cleanup and driver regression checks

The normal compositor shutdown releases its renderer, allocator, and Vulkan
device in that order, after wlroots backends, buffers, and scene listeners.
Historically, nested amdgpu sessions sometimes crashed during explicit Vulkan
cleanup. A dangling `wlr_buffer` destroy-listener bug was subsequently fixed,
so resource teardown is re-enabled. This does **not** establish that all Mesa
or driver combinations are free of lifecycle bugs.

`VELA_SKIP_VULKAN_CLEANUP=1` is an emergency diagnostic opt-out for a
regression on a particular driver: it restores the previous behavior where
remaining Vulkan objects are left to process teardown. Do not enable this
by default or describe the opt-out as a fix for the root cause.

The functional suite checks shutdown with a live client, normal and supervised
logout, repeated clean exits, output power-state changes while locked, and the
explicit opt-out. Every cleanup test asserts the **completion** log, not merely
the announcement that teardown is about to start. On dedicated GPU CI runners,
`VELA_SHUTDOWN_STRESS_CYCLES=30 VELA_SHUTDOWN_VALIDATE=1` enables thirty
consecutive exits and Vulkan validation for these scenarios. Ensure
`VK_LAYER_KHRONOS_validation` is installed: the harness fails when requested
validation cannot be enabled. AMD and NVIDIA self-hosted workflows still use
the **headless** backend and cannot exercise connector power cycling.

Before merging a change to teardown behavior, verify on **AMD RADV, NVIDIA
proprietary, and Intel ANV** as hardware becomes available, and record
driver/Mesa/kernel versions, session type, cycle count and pass/fail:

- Direct DRM/KMS session on a dedicated VT and nested Wayland/X11 session,
  each exiting cleanly with an application still open.
- At least 30 successive login/logout cycles; check that the supervisor
  does not restart the compositor on a voluntary logout.
- Lock, *physically* power the display off/on (including a monitor replug
  where safe), unlock, and log out. Repeat a logout while still locked.
- With `VELA_VULKAN_VALIDATION=1`, confirm the layer is enabled and check for
  errors. Require the `Shutdown: Vulkan resource cleanup complete` line,
  exit code 0 and no compositor-crash message.
- Inspect kernel/driver messages (`journalctl -k -b`) and session/supervisor
  logs for GPU resets, faults or hangs; a clean exit code alone is not enough.

Do not run these physical-display scenarios unattended on a workstation's
active graphical session. Record **not tested** separately from **passed**.
A headless GPU CI pass must never be reported as a direct DRM/KMS pass.
