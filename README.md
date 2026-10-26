<div align="center">

<img src="images/vela_icon.svg" width="96" alt="Vela logo">

# Vela

**A modern, fluid and sharp Linux desktop built for today's hardware.**

Vela has its own Wayland compositor and Vulkan renderer, a Qt Quick shell, and a file
manager and settings app built to match. It uses modern hardware where that improves the
experience — smooth motion, precise rendering, rich effects — and avoids overhead where
it does not. Every animation is tied to your monitor's real frames, and fractional scaling
is designed around physical pixels rather than treated as an afterthought.

[![Build](https://github.com/vela-desktop/vela/actions/workflows/build.yml/badge.svg)](https://github.com/vela-desktop/vela/actions/workflows/build.yml)
![Status](https://img.shields.io/badge/status-alpha-orange)
[![License: GPL v3+](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)
![wlroots](https://img.shields.io/badge/wlroots-0.20-4b8bbe)
![Vulkan](https://img.shields.io/badge/Vulkan-1.4-ac162c)
![Qt](https://img.shields.io/badge/Qt-6-41cd52)
![C++](https://img.shields.io/badge/C%2B%2B-20-00599c)

[Screenshots](#screenshots) · [Try Vela](#try-vela) · [Features](#features) ·
[Install](#install) · [Build](#build-from-source) · [How it works](#how-it-works) ·
[Docs](#documentation) · [Roadmap](docs/roadmap.md)

<br>

<img src="docs/screenshots/start.webp" alt="The Vela desktop with the Start menu open over the file manager" width="100%">

</div>

---

## Why Vela

Desktop hardware and displays have moved fast. A desktop should feel like it belongs on
them. Vela aims for contemporary interaction design, high-refresh motion and precise
rendering without treating visual quality and efficiency as opposing goals.

> **Use hardware when it improves the experience. Avoid overhead when it doesn't.**
> Vela does not remove motion or effects just to win a memory chart. Efficiency comes
> from doing less unnecessary work, not from making the desktop feel less modern.

<table>
<tr>
<td width="50%" valign="top">

### Modern
A coherent shell, polished motion, snap layouts, virtual desktops, quick settings,
notifications and a file manager designed as one desktop rather than a collection of
unrelated utilities. Vela targets current hardware and modern displays on purpose.

</td>
<td width="50%" valign="top">

### Fluid
Animations advance on real vblanks, never arbitrary timers: at 180 Hz they can take 180
steps per second. Late latching draws as late as safely possible before each vblank.
Fullscreen games can go straight to the display through direct scanout, with VRR and
tearing support where appropriate.

</td>
</tr>
<tr>
<td width="50%" valign="top">

### Sharp
Fractional scaling is pixel-exact when an app renders at the screen's scale: its buffer
can be copied 1:1 with no filtering, even at 125% or 175%. An automated test checks this
bit for bit after opening, snapping, maximizing and restoring a window.

</td>
<td width="50%" valign="top">

### Efficient
Efficiency through architecture, not austerity. Damage tracking avoids drawing what did
not change, direct scanout bypasses compositing when possible, and native components stay
responsive without heavyweight runtimes for simple desktop UI. The compositor idles at
**17 MiB** and **0.1% CPU** on the primary development system.

</td>
</tr>
</table>

## Screenshots

<table>
<tr>
<td width="50%"><img src="docs/screenshots/snap.webp" alt="File manager and Settings snapped side by side"><br><sub><b>Snap</b>: windows side by side with Super+arrows or by dragging to an edge or corner.</sub></td>
<td width="50%"><img src="docs/screenshots/task-view.webp" alt="Task View with two virtual desktops"><br><sub><b>Task View</b> (Super+Tab): live window previews and virtual desktops.</sub></td>
</tr>
<tr>
<td width="50%"><img src="docs/screenshots/files.webp" alt="The Vela file manager showing a Pictures folder"><br><sub><b>Files</b>: tabs, breadcrumbs, thumbnails and previews; first frame in about 100 ms.</sub></td>
<td width="50%"><img src="docs/screenshots/alt-tab.webp" alt="Alt+Tab switcher with window previews"><br><sub><b>Alt+Tab</b> with previews, minimized windows included.</sub></td>
</tr>
<tr>
<td width="50%"><img src="docs/screenshots/light.webp" alt="Vela in light mode"><br><sub><b>Light and dark</b> modes for the shell and for apps, switched instantly.</sub></td>
<td width="50%"><img src="docs/screenshots/lock.webp" alt="The Vela lock screen"><br><sub><b>Lock screen</b>: built on ext-session-lock, so a crash never reveals the desktop.</sub></td>
</tr>
</table>

<table>
<tr>
<td width="34%" valign="top"><img src="docs/screenshots/snap-layouts.webp" alt="Snap layouts flyout over the maximize button"><br><sub><b>Snap layouts</b>: hover the maximize button or press Super+Z.</sub></td>
<td width="33%" valign="top"><img src="docs/screenshots/context-menu.webp" alt="Vela desktop context menu"><br><sub><b>Context menus</b> with a compact primary view and KDE service menus under "Show more options".</sub></td>
<td width="33%" valign="top"><img src="docs/screenshots/panels.webp" alt="Quick settings and the notification center"><br><sub><b>Quick settings</b> (Super+A) and the <b>notification center</b> (Super+N).</sub></td>
</tr>
</table>


## Try Vela

> **Alpha software:** Vela is usable as a Wayland session but is not a stable
> release or a replacement for keeping a working desktop/TTY available.
> Hardware-dependent paths need verification on your GPU and driver. Read the
> [hardware coverage and known limitations](docs/hardware.md) before installing.

For a low-risk first look, [build from source](#build-from-source) and
[run nested inside your existing Wayland desktop](#try-it-without-logging-out).
Nested mode is convenient for exploration, but cannot validate physical DRM,
direct scanout or every driver-specific path.

For a full session, use the [Arch package](#arch-linux-cachyos-endeavouros),
the [Fedora RPM](#fedora-44), the [Ubuntu DEB](#ubuntu-2610-experimental)
or [install from source](#other-distributions),
then select Vela at the login
screen. If something fails, [report it](#reporting-a-problem) using
`vela-report`, reviewing any diagnostics before sharing them.

### What Vela owns and what it reuses

Vela implements its own compositor, Vulkan renderer, Qt Quick shell, Files and
Settings. It deliberately reuses standard Wayland protocols and Linux services,
and KDE components such as portals, wallet and themes when beneficial; this does
not mean it runs on KDE's compositor or Plasma shell. Network, Bluetooth,
audio, Xwayland, disk mounting and portals depend on their corresponding
system services (see the [integration table](docs/hardware.md#integration-services)).

## It owns the whole pipeline

A modern shell is only the surface. Vela also controls the path underneath it, from the
moment an app commits a Wayland surface to the moment the pixel lights up on your monitor.

```
input → state → predicted frame → render as late as possible → presentation feedback → correct the model
```

- **Its own scene graph and Vulkan renderer** on top of wlroots, with no `wlr_scene` and
  no wlroots renderer. Flattening, occlusion, damage history with buffer age, damage
  expanded for blur, and per-surface pacing are all Vela's.
- **A frame clock that models time.** Each output keeps a grid of real vblanks and
  measures what frames cost (worst case over the last second). It starts rendering as
  late as it safely can and widens its margin after a miss. It even tells a slow frame
  apart from constant latency added by a host compositor.
- **A slow app can't hold the screen back.** A commit whose buffer the app's GPU is
  still drawing waits for its fence (explicit or implicit sync), and until then the
  screen keeps the app's previous frame: the cursor, the animations and every other
  window stay on the vblank. Buffers stay imported only while they're on screen, because
  the kernel would otherwise make every frame wait for each app redrawing one of them.
  Measured with an app whose GPU finishes 150 ms late: 0 missed vblanks, down from 47.
- **Direct scanout done properly.** A single opaque surface covering the output 1:1
  goes straight to the display plane, with dmabuf feedback and explicit-sync timelines.
  The moment that stops being true, Vela falls back to compositing.
- **Fractional scaling it can prove.** Exact preferred scales, pixel-snapped
  positions, and nearest sampling whenever a buffer maps 1:1, verified bit for bit by
  an automated test.

The whole design is written down in [docs/renderer.md](docs/renderer.md).

> [!NOTE]
> **Vela is alpha software.** You can already pick it at the login screen and use it
> every day: compositor, shell, file manager, settings, lock screen. The complete GPU-
> dependent functional and sharpness suites are run on the primary development machine
> and currently pass; GitHub-hosted CI skips them because it has no usable DRM/Vulkan
> device. Some hardware-specific paths still rely on manual verification. The interface
> speaks **English and Italian** (Settings → Time & language), switched live.

## Features

### Window management and multitasking
- **Snap layouts**: halves, quarters, and six layouts (thirds, 2/3 + 1/3,
  and more). **Snap Assist** offers your other windows for the empty space. Windows you
  arrange this way form a **snap group** that the taskbar shows and restores together.
- **Task View and virtual desktops** (Super+Tab): drag windows between desktops,
  rename and reorder desktops, and switch with Super+Ctrl+←/→ and a sliding animation.
  Your desktops are remembered across sessions.
- **Alt+Tab** with live previews, including minimized windows. A quick Alt+Tab just
  switches without showing the panel.
- **Window animations**: windows fade and rise as they open, shrink away as they close,
  and fly into their taskbar button when minimized.
- **Rounded corners and soft shadows**, drawn by the compositor and sharp at every
  scale. They turn off when a window is maximized, snapped or fullscreen.
- **Vela's own title bar** for apps that accept server-side decorations (Qt, KDE, X11),
  with the app icon and a wallpaper-tinted translucent surface. Apps that draw their own
  title bar, such as Chromium with tabs on top, keep theirs.
- Invisible resize borders, Super+drag to move, Super+right-drag to resize, and a window
  menu (Alt+Space) with keyboard move and resize.

### Shell
- **Taskbar**: pinned and running apps centered (or left-aligned), drag to reorder, jump
  lists with recent files, **hover previews**, a system tray (StatusNotifierItem), and
  one taskbar per monitor.
- **App launcher**: instant search, pinned apps, keyboard navigation, and a Super+X power-user menu.
- **Quick settings** (Super+A): Wi-Fi network picker, Bluetooth, airplane mode, power
  mode, Night light, accessibility, brightness, volume and battery, and a sound page
  (Super+Ctrl+V) to pick the output, see Bluetooth headsets' charge, and set each app's
  volume and output.
- **Media, volume and microphone keys** that work whatever has the focus, with an
  indicator; the wheel over an app's taskbar button sets that app's volume.
- **Notifications** with actions, a **notification center** with a calendar (Super+N),
  and Do Not Disturb.
- **Clipboard history** (Super+V): text and images, pinned items that survive a reboot,
  and one-click paste into the focused app.
- **Snipping tool** (Super+Shift+S or PrtSc): capture a rectangle, a window or a full
  screen, straight to the clipboard and to `Pictures/Screenshots`.
- **Desktop icons** with thumbnails, drag and drop to and from apps, Properties, and
  undo. Also **Run** (Super+R) and **Show desktop** (Super+D).
- **Light and dark** modes and a coordinated accent palette, applied to KDE and GTK apps
  too.

### Files (`vela-files`)
- Tabs, breadcrumb address bar, search in subfolders, Details and icon views (remembered
  per folder), and a **preview pane** (images, text, video and PDF thumbnails).
- Background copy and move with progress and conflict resolution, drag and drop
  everywhere, ZIP/7z/TAR, shortcuts, Favorites, Trash with restore.
- **Atomic file replacement and staged copies.** Each file is written to a hidden temporary
  next to its destination and renamed into place only once complete. Replacing a file keeps
  the previous contents until the new copy is safely written, so a full disk or read error
  does not turn a good destination file into a partial one.
- **Unmounted drives** (udisks) in This PC: double-click to mount, eject from the menu.

### Settings (`vela-settings`)
- **Displays**: drag-and-drop arrangement, scale, resolution, refresh rate, rotation, and
  "Keep these settings?" with compositor-owned revert on timeout or a Settings crash.
  Native Wayland configuration requires no external display utility. Also **Night light**, variable refresh
  rate and tearing.
- **Sound, Notifications, Power, Clipboard**, Bluetooth and devices, **mouse and
  touchpad** (tap to click, scroll direction, three- and four-finger gestures), and
  Network.
- **Personalization**: wallpaper, colors, taskbar. **Apps**: installed and default apps.
  **Time and language**: date and time, keyboard layouts.
- **Accessibility**: magnifier, color filters, sticky keys.

### Graphics and gaming
- Vela's **own scene graph and Vulkan 1.4 renderer**: no `wlr_scene`, no GL fallback.
- **Live acrylic blur** (dual Kawase) behind the shell, exposed to apps through the
  standard `ext-background-effect-v1` protocol.
- **Direct scanout** for fullscreen apps, **VRR** (FreeSync/G-Sync, automatic for
  fullscreen apps by default), **tearing control** for games, explicit sync
  (`linux-drm-syncobj-v1`), relative pointer and pointer constraints.
- **Night light** in the monitor's gamma LUT, so screenshots stay untinted and games keep
  direct scanout. It can follow sunset and sunrise, computed offline from your time zone.
- Picks the **highest refresh rate** at native resolution, even when the monitor
  advertises 60 Hz as "preferred".
- **Xwayland** on demand for Steam, games and older apps.

### Accessibility and input
- Magnifier (Super+Plus), grayscale and color-blindness filters (Super+Ctrl+C), and sticky
  keys.
- Familiar touchpad defaults and gestures: three or four fingers up for Task View,
  down for the desktop, sideways to switch apps or desktops.

### A session you can rely on
- **Crash recovery.** A tiny supervisor holds the Wayland socket. If the compositor
  crashes, a new one starts on the same socket and Qt/KDE apps reconnect and reappear,
  as with KWin. If the screen was locked, Vela comes back **locked**. Three crashes in a
  minute end the session cleanly.
- If the shell crashes, the compositor restarts it, and your windows stay open.
- A **secure lock screen** (`ext-session-lock-v1`): if the locker dies, the screen stays
  black and locked, and the locker is restarted.
- **User Account Control.** When an app needs administrator rights, the whole desktop
  dims and blurs and one dialog asks: who is asking, for what, and the password. Nothing
  else takes the keyboard until you answer; requests queue up instead of being refused
  ([design](docs/polkit-agent.md)).
- **Integrates with KDE's services**: portals, the KDE wallet (so Chrome, Brave and VS
  Code keep their logins), color schemes and icon themes.

## Install

Vela needs a GPU with **Vulkan 1.4**: AMD or Intel with Mesa ≥ 25.0, or NVIDIA with the proprietary driver ≥ 570 or NVK. Sound needs **PipeWire and WirePlumber**. Once installed, choose **Vela** from the login screen.

### Arch Linux, CachyOS, EndeavourOS

```sh
sudo pacman -S --needed git curl base-devel cosign
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update
```

### Fedora 44

Only Fedora 44 x86_64 is currently supported by the native RPM updater; other Fedora versions and immutable variants are not supported by this installer.

```sh
sudo dnf install git curl rpm-build dnf5-plugins
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update
```

### Ubuntu 26.10 (experimental)

Ubuntu 26.10 with the **universe** repository enabled is supported by an
experimental native DEB updater. It builds from the selected Git commit
locally and does **not** automatically install unsigned CI previews.

```sh
sudo apt update
sudo apt install git ca-certificates sudo
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update
```

### Other distributions

See [build from source](#build-from-source); there is currently no native package updater for these systems.

`vela-update` installs Vela on first run and handles subsequent updates. On Arch and Fedora it uses Sigstore/Cosign to verify downloaded packages (no GitHub CLI required), falling back to a local build when necessary. On Ubuntu 26.10 it always builds a local DEB. Fedora 44 users should install Cosign from its official upstream RPM before the first download; see the [installation and update guide](docs/installation.md) for update options, test branches, rollback and troubleshooting.
## Reporting a problem

Run `vela-report` to create a diagnostic ZIP with a description and the evidence
you choose. It explains each collection, lets you review or remove files, and
asks before exporting. Nothing is uploaded. Use `vela-report --cli` from a TTY
or SSH session; the graphical interface also works in other desktop environments.
See [Vela Report](docs/reporting.md) for privacy details, options and limits.

## Build from source

The source build requires wlroots **0.20**, Qt **6.7+**, Vulkan headers and loader, and the other dependencies listed in the [developer guide](docs/development.md#distribution-dependencies).

```sh
cmake -B build -G Ninja
cmake --build build
```

To install system-wide or run a nested session, follow the [installation guide](docs/installation.md#other-distributions) or the instructions below.
### Try it without logging out

From a running Wayland session (Plasma, for example), Vela opens in a window like a
virtual monitor:

```sh
sh scripts/run-nested.sh
```

The host desktop keeps the Super key, so many shortcuts have Alt variants,
but some do not (see [the shortcut reference](docs/shortcuts.md)).
These Alt variants are only active when nested. In a real session, Alt+letter stays with
your apps for their menu mnemonics.

## Keyboard shortcuts

Vela uses familiar desktop shortcuts: <kbd>Super</kbd> opens the app launcher,
<kbd>Super</kbd>+<kbd>Tab</kbd> opens Task View, <kbd>Super</kbd>+arrow keys snap windows,
and <kbd>Super</kbd>+<kbd>A</kbd>/<kbd>N</kbd>/<kbd>V</kbd> open quick settings,
notifications and clipboard history.

When Vela runs nested inside another Wayland desktop, Alt-based alternatives are enabled
for shortcuts that the host would otherwise capture.

See **[docs/shortcuts.md](docs/shortcuts.md)** for the complete normal and nested-session
shortcut reference.

## How it works

```mermaid
flowchart TB
    DM["Login screen<br/>(SDDM, Plasma Login)"] --> S["vela-session"]
    S --> SUP["Supervisor<br/><i>holds the Wayland socket,<br/>restarts the compositor</i>"]
    SUP --> C["vela-compositor<br/>C17 · wlroots 0.20<br/>own scene graph · Vulkan 1.4 renderer"]
    C -- "layer-shell · foreign-toplevel<br/>ext-background-effect · image capture" --> SH["vela-shell<br/>Qt Quick: taskbar, Start, panels, desktop"]
    C --> APPS["Apps<br/>Wayland and Xwayland"]
    C --> F["vela-files"]
    C --> SET["vela-settings"]
    C -- "ext-session-lock" --> L["vela-lock"]
    C --> HW["DRM/KMS · libinput · GBM"]
```

- **The compositor** owns the screen (see [above](#it-owns-the-whole-pipeline)). Its
  Vulkan 1.4 renderer draws SDF rounded corners, analytic two-layer shadows, dual Kawase
  blur and night light in the gamma LUT. It also implements `wlr_renderer`, so wlroots'
  cursor, screencopy and shm uploads share the same device.
- **The shell** is a separate Qt Quick process using LayerShellQt. It talks to the
  compositor through standard Wayland protocols, plus a small command socket (for
  example, Super opens Start). A shell crash never takes your windows down.
- **Motion design lives in one place**: `compositor/src/motion.h` and
  `common/controls/Theme.qml` share the same cubic-bezier(0, 0, 0.2, 1) curve.
- **The session** declares itself `XDG_CURRENT_DESKTOP=Vela:KDE`. Qt/KDE apps use KDE's
  theme, and browsers and Electron apps use KDE's wallet, exactly as inside Plasma. It
  connects to systemd (`vela-session.target`), starts the portals and its own polkit
  agent, and keeps the last two session logs in `~/.local/state/vela/`.

## Performance

See [how to reproduce and interpret these measurements](docs/performance.md),
including current validation limits. These are historical observations on the
primary development machine, **not independently repeated for this README update**.

Vela does not chase low resource usage by stripping away the experience. It measures
overhead so visual ambition does not quietly turn into waste:


| Goal | Target | Previously observed |
|---|---|---|
| Start menu | visible on the next frame after the key | ✔ |
| Context menus | visible on the next frame after the click | ✔ |
| Files cold start | first frame within 150 ms | **104 ms** (median of 5) |
| Animations | always driven by real vblanks, never timers | ✔ |
| Compositor at idle | low overhead | **17 MiB PSS**, **0.1% of one CPU**, 3–7 voluntary context switches/s |
| Shell at idle | low overhead | **143 MiB PSS**, **0.1% of one CPU**, 1–4 voluntary context switches/s |

These measurements are from Vela's primary development system (Ryzen 9 9950X, Radeon
RX 9070 XT, CachyOS, Mesa/RADV). Numbers come from `scripts/measure.sh`, which samples
PSS memory, idle CPU, voluntary context switches per second (a limited proxy,
not the total number of process wakeups) and, on battery, average power draw
of the whole computer over 30 seconds. `--json` prints one line to compare over
time. Record the exact commit and driver versions alongside each published run.

## Development

Vela can be run nested inside the current Wayland session or headlessly for automated
work:

```sh
sh scripts/run-nested.sh
sh scripts/run-headless.sh
```

Build and run the complete CTest suite with:

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The GPU-dependent functional and sharpness suites use Vela's real Vulkan renderer
on self-hosted hardware; GitHub-hosted CI cannot run them. See the
[GPU CI policy](docs/ci-quality.md#gpu-ci-policy) for when AMD and NVIDIA
results are required and how the candidate revision is verified.

For the full workflow, test harness, command socket, debug switches and project layout,
see the documentation below.

## Documentation

| Document | Contents |
|---|---|
| **[Install and update](docs/installation.md)** | Installation, verified updates, test branches and rollback. |
| **[Alpha hardware coverage](docs/hardware.md)** | Driver validation scope, optional services and manual smoke tests. |
| **[Performance methodology](docs/performance.md)** | Measurements, precise metric definitions, reproduction and limitations. |
| **[Roadmap](docs/roadmap.md)** | Work in progress, what comes next, what is not planned, and what is done. |
| **[Contributing](CONTRIBUTING.md)** | Development workflow, PR checklist, testing and DCO. |
| **[Security](SECURITY.md)** | Private vulnerability reporting and alpha support policy. |
| **[Renderer](docs/renderer.md)** | Scene graph, Vulkan renderer, scaling, damage, frame scheduling and direct scanout. |
| **[Polkit agent](docs/polkit-agent.md)** | The User Account Control dialog: agent, dialog, protocol, security notes and tests. |
| **[Audio](docs/audio.md)** | Design of the shared audio backend on WirePlumber: model, volume rules, levels, microphone in use, tests. |
| **[Vela Report](docs/reporting.md)** | Guided diagnostic ZIPs, CLI and GUI, collection choices, privacy and validation. |
| **[Development](docs/development.md)** | Build options, nested/headless workflows, development tools and repository layout. |
| **[Testing](docs/testing.md)** | Unit, functional and sharpness suites; GPU requirements; local and CI coverage. |
| **[Configuration](docs/configuration.md)** | `vela.conf`, display persistence, runtime overrides and renderer/debug environment variables. |
| **[Keyboard shortcuts](docs/shortcuts.md)** | Complete normal-session and nested-session shortcut reference. |

## Contributing

Vela is a young project and moves fast. Issues and ideas are welcome.
See the [contribution guide](CONTRIBUTING.md) and [security policy](SECURITY.md). Before larger
changes, open an issue to talk about it. Use English for code, new comments, commit messages and interface text.
Some older comments are in Italian; contributions translating them are welcome.
Please sign off your commits (`git commit -s`, the
[Developer Certificate of Origin](https://developercertificate.org/)): contributions
are accepted under the project's license, GPL-3.0-or-later.

## AI Disclosure

AI tools are actively used in Vela's development, including code generation,
refactoring, debugging, code review, and documentation.

The project's vision, architecture, and technical direction remain human-driven.
AI-assisted code is not inherently trusted: it is subject to the same review,
testing, and quality expectations as any other contribution.

AI is a development tool, not a substitute for engineering judgment.

## Acknowledgements

Vela stands on the shoulders of [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots),
[Qt](https://www.qt.io/), [KDE Frameworks and Plasma](https://kde.org/) (LayerShellQt,
Breeze icons, the portals and the wallet), [Mesa](https://mesa3d.org/) and the Wayland
protocols community.

## License

Vela is free software: you can redistribute it and/or modify it under the terms of the
[GNU General Public License](LICENSE) as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

Every source file and the artwork in `images/` carry an
`SPDX-License-Identifier: GPL-3.0-or-later` header. The Wayland protocol files in
`protocols/` keep their original MIT-style licenses.
