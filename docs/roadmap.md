# Roadmap

Where Vela is, what is being built now, and where it is going. This is a
long-term roadmap: it describes the desktop Vela wants to become, not a
schedule, and some of it is years of work. Each larger piece
of work starts with a design document in `docs/`, which holds its decisions,
stages and tests; this page only links to it and says where it stands.

Vela is **alpha**, and its first beta, 0.1, is being prepared. "Done" means
implemented and tested, not validated on every GPU and driver: see
[hardware coverage](hardware.md) for the current limits.

## Beta 0.1

The first beta takes the desktop under **Done** and makes it solid, rather
than adding to it. It is Vela's first versioned release, with release notes,
alongside the per-commit alphas.

- **Feature freeze: 27 October 2026.** From that day `main` takes only bug
  fixes, tests, CI, packaging, documentation and translations until 0.1 is
  released.
- **The one exception** is the Updates page
  ([#133](https://github.com/vela-desktop/vela/pull/133)), which can still
  merge after the freeze.
- Every other feature pull request stays open and keeps being reviewed, but
  merges after 0.1. The same goes for everything under **Next**.

## In progress

| Work | Design | Pull request | Status | Release |
|---|---|---|---|---|
| **Updates page**: Settings → System → Updates checks for a newer Vela with `vela-update` and starts the update in a terminal | in the PR | [#133](https://github.com/vela-desktop/vela/pull/133) | draft | 0.1 |
| **Touch and pen**: touch in every app and in the shell, edge gestures, the pen over `tablet-v2`, tablet mode | in the PR | [#94](https://github.com/vela-desktop/vela/pull/94) | design under discussion | after 0.1 |
| **On-screen keyboard**: `vela-onscreen-keyboard`, opened when a text field is focused by touch | in the PR | [#95](https://github.com/vela-desktop/vela/pull/95) | design under discussion | after 0.1 |
| **Snipping tool**: text from a capture (OCR), a delay timer, a color picker and screen recording | in the PR | [#118](https://github.com/vela-desktop/vela/pull/118) | draft | after 0.1 |
| **Screen sharing indicator**: a taskbar icon while the screen is shared, with notifications held back | in the PR | [#72](https://github.com/vela-desktop/vela/pull/72) | draft | after 0.1 |
| **Always on top**, from the window menu | in the PR | [#108](https://github.com/vela-desktop/vela/pull/108) | draft | after 0.1 |

## Next

Everything Vela means to do after the work in progress, grouped by area and in
no particular order. There are no dates, and an item listed here is a
direction, not a promise.

### Graphics and displays
- **Color**: 10-bit outputs, HDR and `color-management-v1` (renderer
  [stage S7](renderer.md#11-stages)). The renderer already blends in linear
  space, so this extends it rather than rewriting it ([§7.5](renderer.md#75-color)).
- ICC profiles per monitor, and the brightness slider's meaning in HDR
  ([#45](https://github.com/vela-desktop/vela/pull/45)).
- VRR policy during animations ([renderer §4.4](renderer.md#44-vrr)).
- Live blur behind Vela's title bar, which uses a solid tint today, and a cache
  for blurred backgrounds that don't change.
- A setting that lets X11 apps scale themselves at fractional scales, sharp
  but dependent on each app ([renderer §3.9](renderer.md#39-x11-apps)).
- Hybrid graphics: laptops with two GPUs, outputs on the second GPU, choosing
  the GPU per app.
- Projection shortcut (Super+P): duplicate, extend, second screen only.
- Mirrored outputs.
- More Wayland protocols, such as `fifo`, `commit-timing`, `security-context`,
  `xdg-foreign`, `xdg-toplevel-icon` and `xdg-dialog`.

### Windows and multitasking
- Window rules per app: position, size, desktop, monitor.
- Windows that reopen where they were, and session restore after login.
- Moving a window to another monitor or desktop from the keyboard.
- Per-monitor virtual desktops, as an option.
- Picture-in-picture for video players.

### Shell
- **Input methods and an emoji panel**: `text-input` and `input-method`, with
  an emoji picker on Super+. .
- **Start menu search** across files and settings, and Recommended files.
- Folders in the Start menu's pinned apps.
- Widgets: a board with weather, calendar, news and system status.
- Taskbar: progress and unread badges on app buttons, auto-hide, vertical
  taskbar, never combine buttons.
- Notifications grouped by app, and Do Not Disturb turned on automatically
  during fullscreen games.
- Calendar events in the notification center.
- Brightness keys with an on-screen indicator, on the monitor under the pointer.
- Keyboard backlight control.

### Snipping tool
- Annotations (pen, highlighter, arrows, crop) before saving.

### Settings
- Automatic update checks, with a notification when a new version is out.
- Users and accounts: adding users, picture, password, automatic login.
- Printers and scanners.
- A keyboard shortcut editor.
- Fonts, and regional formats for dates, numbers and units.
- Battery: charge limit to protect the battery, battery health.
- Power policy: idle dimming, battery profiles, ambient light.

### Audio
- Ducking: lowering other sounds during a call ([audio §10](audio.md#10-not-in-scope)).
- System sounds: login, notifications, device plugged in, with a volume of
  their own.

### Files
- Network locations: SMB, SFTP, WebDAV.
- Phones and cameras (MTP, PTP).
- An indexed search that finds files by name and content.
- Batch rename.
- Mounting disk images.
- Tags.

### Vela's own apps
- Task Manager (Ctrl+Shift+Esc): processes, CPU, GPU, memory, disk, network
  and startup apps.
- A text editor, an image viewer, a terminal and a calculator that match the
  rest of Vela.

### Languages
- More languages, translated on a platform such as Weblate.

### Distribution
- A pacman repository, so updates come with the system's.
- Packages for Debian and Ubuntu.
- A live image or virtual machine to try Vela without installing it.
- A notification after a crash that offers `vela-report`.

## Not planned

Decisions already taken. Each design document explains why.

- **A GL or software renderer.** Vela needs Vulkan 1.4 and has no fallback
  ([renderer §2](renderer.md#2-decisions-taken)).
- **Fewer effects on battery.** There is no automatic power-saving mode that
  turns off blur or animations ([renderer §2](renderer.md#2-decisions-taken)).
- **PulseAudio without PipeWire**, and `libpulse` as a backend
  ([audio §2](audio.md#2-decisions-taken)).
- **Echo cancellation, noise suppression, mono audio, spatial sound and audio
  "enhancements"** ([audio §10](audio.md#10-not-in-scope)).
- **Software dimming in SDR** for monitors without DDC/CI: it costs contrast
  and saves no power ([#45](https://github.com/vela-desktop/vela/pull/45)).
- **One brightness slider for all monitors** ([#45](https://github.com/vela-desktop/vela/pull/45)).
- **32-bit builds.**

## Done

- **Session**: login entry, systemd and D-Bus, portals, Xwayland, lock screen,
  idle, notifications, system tray, and Vela's own
  [polkit agent](polkit-agent.md) (User Account Control).
- **Compositor and renderer**: a [C core](c-core.md), Vela's own scene and
  [Vulkan renderer](renderer.md), pixel-exact fractional scaling, late
  latching, live acrylic blur, rounded corners and shadows, Vela's title bar.
- **Displays and gaming**: native display configuration with compositor-owned
  rollback, multi-monitor hotplug with per-output scale, direct scanout, VRR,
  tearing control, explicit sync, Night Light, and
  [brightness](brightness.md) per monitor over DDC/CI and the kernel backlight
  (its checks on real monitors, [§12](brightness.md#12-tests), remain).
- **Desktop**: Settings, Files (with a quick preview on Space: pictures, PDFs,
  text, videos and sounds), desktop icons, virtual desktops, Task View,
  snap layouts and Snap Assist, modern context menus, light mode, a
  show-desktop strip, Caffeine, and file transfers in the taskbar.
- **[Visual coherence](visual-coherence.md)**: the same pixels, clock and
  motion across compositor, shell and apps, and shared controls for all of
  Vela's interfaces.
- **Daily comfort**: clipboard history, snipping tool, touchpad gestures,
  mouse and touchpad settings.
- **Audio**: one [native backend](audio.md) for the shell and Settings, with
  per-app inputs and outputs, levels, microphone in use, device profiles,
  codecs and sample rates.
- **Reliability and security**: compositor and shell crash recovery,
  [`vela-report`](reporting.md) for guided bug reports, `vela-update`,
  [signed packages with verified provenance](package-provenance.md) for Arch
  and Fedora, and CI with sanitizers, static analysis and real AMD and NVIDIA
  GPUs.
- **Languages**: English and Italian, switched live from Settings.

## Keeping this page current

This page changes in the same pull request as the work it describes:

- A new design document adds its row to **In progress**.
- The pull request that finishes the last stage moves that work to **Done**.
- Each row in **In progress** names the release it targets.
- Something from **Next** gets a design document before it moves to **In progress**.

To propose a feature, open an issue that describes the behavior first
(see [CONTRIBUTING](../CONTRIBUTING.md)).
