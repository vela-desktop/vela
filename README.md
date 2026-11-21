<div align="center">

<img src="images/vela_icon.svg" width="96" alt="Vela logo">

# Vela

**A modern, fluid and sharp Linux desktop built for today's hardware.**

Vela is a complete desktop environment for Wayland: its own compositor and Vulkan
renderer, a Qt Quick shell, a file manager and a settings app, designed together.

[![Build](https://github.com/vela-desktop/vela/actions/workflows/build.yml/badge.svg)](https://github.com/vela-desktop/vela/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/vela-desktop/vela?label=release&color=yellow)](https://github.com/vela-desktop/vela/releases)
[![License: GPL v3+](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)

[Website](https://velade.org) · [Get Vela](#get-vela) · [Features](docs/features.md) ·
[What's new](CHANGELOG.md) · [How it works](#how-it-works) ·
[Documentation](#documentation) · [Community](#community) · [Roadmap](docs/roadmap.md)

<br>

<img src="docs/screenshots/files.webp" alt="The Vela desktop with the file manager open" width="100%">

</div>

> [!NOTE]
> **Vela is beta software.** You can pick it at the login screen and use it every day,
> but keep a working desktop or TTY available. Packages are available for Arch Linux
> (and CachyOS, EndeavourOS), Fedora 44 and 45, and Ubuntu 26.10. It is tested on AMD
> and NVIDIA GPUs, and on Intel in experimental CI; read the
> [hardware coverage and known limitations](docs/hardware.md) before installing. The
> interface is available in English and Italian.

## Why Vela

- **Small text stays clear in motion.** The shell, Files and Settings use scalable
  glyphs and grayscale antialiasing. A [scrolling test](common/tests/text_rendering.cpp)
  moves text through one physical pixel in 64 steps and checks for colored fringes.
- **A 125% desktop, a native 1440p game.** Windows drawn at the monitor's scale map
  1:1 to its pixels, checked bit for bit. Even an [X11 game](docs/renderer.md#39-x11-apps)
  can use all 2560×1440 pixels and direct scanout.
- **Each monitor sets the pace.** Switch desktops across 75 Hz and 180 Hz screens.
  Each draws the animation for its own next refresh. Recent frame times tell
  Vela how late it can start drawing.
- **A slow window can wait.** While an app's GPU work finishes, its previous frame
  stays visible. The cursor keeps moving, even in a [test](tests/functional/test_ready.py)
  that delays the app by 150 ms.

## What's inside

- **Compositor:** stacking window management with window snapping, virtual desktops, live
  window previews, blur, rounded corners and shadows. Xwayland runs X11 apps and games.
  Fullscreen games can use variable refresh rate and tearing.
- **Shell:** taskbar, app launcher, quick settings, notifications, clipboard history,
  screenshots and desktop icons, in light and dark modes.
- **Themes** (in 0.2, on the Experimental channel today): complete looks in one click,
  Vela Expressive among them, with an accent that can follow your wallpaper's colors.
- **Files:** tabs, search, previews, archives, removable drives and safe background copies.
- **Settings:** displays, sound, network, Bluetooth, input, power, personalization,
  accessibility and updates.
- **Session:** a secure lock screen, an authorization prompt for administrator actions,
  and recovery from compositor, shell and GPU crashes. Windows stay open after a shell
  crash; after a compositor or GPU restart, Qt and KDE apps reconnect.

See the [feature gallery](docs/features.md#screenshots) for a closer look at the
desktop, from the corners of Quick Settings to the lock-screen clock.

## Get Vela

Vela needs a GPU with **Vulkan 1.4** (AMD or Intel with Mesa ≥ 25.0, NVIDIA with the
proprietary driver ≥ 570 or NVK) and **PipeWire with WirePlumber** for sound. From 0.2
(on the Experimental channel today) it also needs an **x86-64-v3 CPU**: roughly
2017-generation hardware, though the instruction set, not the date, is what counts.
CPU and GPU requirements are independent.

Install the prerequisites for your distribution:

| Distribution | Prerequisites |
|---|---|
| Arch Linux, CachyOS, EndeavourOS | `sudo pacman -S --needed git curl base-devel cosign` |
| Fedora 44 and 45 | `sudo dnf install git curl rpm-build dnf5-plugins`, plus the [upstream Cosign RPM](docs/installation.md#fedora-44-and-45) |
| Ubuntu 26.10 (with *universe*) | `sudo apt install git ca-certificates sudo cosign` |

Then install the current Beta and choose **Vela** at the login screen:

```sh
git clone https://github.com/vela-desktop/vela.git
sh vela/packaging/vela-update --channel beta
```

Beta packages are prebuilt, signed and verified with Cosign before installing. The
[installation guide](docs/installation.md) covers the Experimental channel, updates,
rollback and [other distributions](docs/installation.md#other-distributions).

**Just want a look?** [Build from source](docs/development.md#build) and run
`sh scripts/run-nested.sh`: Vela opens in a window inside your current Wayland desktop.
Nested mode is good for exploring, but cannot validate every hardware path. For a full
session without touching your system, run Vela in a
[QEMU/KVM virtual machine with Venus](docs/virtual-machines.md) (experimental; it needs
the Experimental channel).

## How it works

```mermaid
flowchart TB
    DM["Login screen"] --> S["vela-session"]
    S --> SUP["Supervisor<br/><i>holds the Wayland socket,<br/>restarts the compositor</i>"]
    SUP --> C["vela-compositor<br/>C17 · wlroots 0.20<br/>own scene graph · Vulkan 1.4 renderer"]
    C -- "standard Wayland protocols" --> SH["vela-shell<br/>Qt Quick"]
    C --> APPS["Apps<br/>Wayland and Xwayland<br/>(vela-files, vela-settings...)"]
    C -- "ext-session-lock" --> L["vela-lock"]
    C -- "starts and restarts" --> HELP["Helpers<br/>vela-polkit-agent · vela-bluetooth<br/>vela-brightness (one per display)<br/>vela-update-daemon"]
    APPS -- "screen sharing, file choosers" --> P["xdg-desktop-portal<br/>wlr · GTK"]
    P -- "capture protocols" --> C
    C --> HW["DRM/KMS · libinput · GBM"]
```

- **The compositor** is built on wlroots with its own scene graph and Vulkan renderer:
  it controls the whole path from an app's commit to the pixel on the monitor, including
  frame scheduling, damage tracking and direct scanout ([renderer design](docs/renderer.md)).
- **The shell** is a separate Qt Quick process that talks to the compositor through
  standard Wayland protocols, so a shell crash never takes your windows down.
- **Small helpers** do the work that touches hardware or system services: the
  authorization prompt's agent, Bluetooth, monitor brightness over DDC/CI and the
  update checks. Each is its own process, started and restarted by the compositor, so
  a slow monitor or a stuck D-Bus call never delays a frame.
- **Vela reuses** standard Linux services and selected KDE components (themes and
  icons), and keeps app passwords in KWallet or GNOME Keyring, rather than reinventing
  them. It does not run on KDE's compositor or Plasma shell
  ([integration services](docs/hardware.md#integration-services)).

## Get involved

Vela is a young project and moves fast. Every kind of help is welcome:

- **Try it and report problems.** `vela-report` builds a diagnostic ZIP that you review
  before sharing; nothing is uploaded ([details](docs/reporting.md)).
- **Test on your hardware.** GPU, driver and multi-monitor results are especially
  valuable ([smoke tests](docs/hardware.md#suggested-manual-smoke-tests)).
- **Translate.** New languages are welcome ([how translations work](docs/development.md#translations)).
- **Contribute code.** Read the [contribution guide](CONTRIBUTING.md) and open an issue
  before larger changes. Security issues go through the [security policy](SECURITY.md).

## Community

Join the [Vela Desktop community on Matrix](https://matrix.to/#/#vela:velade.org)
for discussions, support, announcements and development. Open the Space Lobby to
browse and join the rooms you're interested in. An account from any federated
Matrix homeserver works; no `velade.org` account is required.

## Documentation

[Features](docs/features.md) · [Installation and updates](docs/installation.md) ·
[Keyboard shortcuts](docs/shortcuts.md) · [Hardware coverage](docs/hardware.md) ·
[Development guide](docs/development.md) · [Renderer design](docs/renderer.md)

Everything else, from configuration to the design documents, is in the
[documentation index](docs/README.md).

## AI disclosure

AI tools are actively used in Vela's development, including code generation, review and
documentation. The project's vision, architecture and technical direction remain
human-driven, and AI-assisted code is held to the same review, testing and quality
expectations as any other contribution.

## Acknowledgements and license

Vela stands on the shoulders of [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots),
[Qt](https://www.qt.io/), [KDE Frameworks and Plasma](https://kde.org/),
[Mesa](https://mesa3d.org/) and the Wayland protocols community.

Vela is free software under the [GNU General Public License](LICENSE), version 3 or
(at your option) any later version. The Wayland protocol files in `protocols/` keep
their original MIT-style licenses.
