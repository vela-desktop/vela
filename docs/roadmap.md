# Roadmap

Where Vela is, what is being built now, and where it is going. This is a
long-term roadmap: it describes the desktop Vela wants to become, not a
schedule, and some of it is years of work. Each larger piece
of work starts with a design document in `docs/`, which holds its decisions,
stages and tests; this page only links to it and says where it stands.

Vela is **alpha**. "Done" means implemented and tested, not validated on every
GPU and driver: see [hardware coverage](hardware.md) for the current limits.

## In progress

| Work | Design | Pull request | Status |
|---|---|---|---|
| **Display brightness**: a slider per monitor, over DDC/CI for external monitors and the kernel backlight for built-in screens | in the PR | [#45](https://github.com/vela-desktop/vela/pull/45) | design done; stages B0–B4 to do |
| **On-screen keyboard**: typing without a keyboard in every app and on the lock screen, `text-input-v3`, and the groundwork for input methods | [on-screen keyboard](onscreen-keyboard.md) | — | design draft |
| **Visual coherence**: the same pixels, clock and motion across compositor, shell and apps | in the PR | [#40](https://github.com/vela-desktop/vela/pull/40) | steps 1–3 of 11 done |

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
- Touchscreens: touch gestures, automatic rotation from the orientation sensor.
- Drawing tablets and pens (`tablet-v2`), with a Settings page.
- More Wayland protocols, such as `fifo`, `commit-timing`, `security-context`,
  `xdg-foreign`, `xdg-toplevel-icon` and `xdg-dialog`.

### Windows and multitasking
- Always on top, from the window menu.
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
  during fullscreen games and screen sharing.
- Calendar events in the notification center.
- An indicator when the screen is being shared or recorded.
- Brightness keys with an on-screen indicator, on the monitor under the pointer.
- Keyboard backlight control.

### Snipping tool
- **Screen recording**, with or without sound.
- Annotations (pen, highlighter, arrows, crop) before saving.
- A delay timer.
- Copying text from a capture (OCR).
- A color picker.

### Settings
- An Updates page, with a notification when a new version is out.
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
- Quick preview with Space.
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
- Versioned releases with release notes, alongside the per-commit alphas.
- A pacman repository, so updates come with the system's.
- Packages for Fedora, and Debian/Ubuntu.
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
  tearing control, explicit sync, Night Light in the gamma LUT.
- **Desktop**: Settings, Files, desktop icons, virtual desktops, Task View,
  snap layouts and Snap Assist, modern context menus, light mode.
- **Daily comfort**: clipboard history, snipping tool, touchpad gestures,
  mouse and touchpad settings.
- **Audio**: one [native backend](audio.md) for the shell and Settings, with
  per-app inputs and outputs, levels, microphone in use, device profiles,
  codecs and sample rates.
- **Reliability and security**: compositor and shell crash recovery,
  [`vela-report`](reporting.md) for guided bug reports, `vela-update`,
  [signed packages with verified provenance](package-provenance.md), and CI
  with sanitizers and static analysis.
- **Languages**: English and Italian, switched live from Settings.

## Keeping this page current

This page changes in the same pull request as the work it describes:

- A new design document adds its row to **In progress**.
- The pull request that finishes the last stage moves that work to **Done**.
- Something from **Next** gets a design document before it moves to **In progress**.

To propose a feature, open an issue that describes the behavior first
(see [CONTRIBUTING](../CONTRIBUTING.md)).
