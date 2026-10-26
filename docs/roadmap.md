# Roadmap

Where Vela is, what is being built now, and where it is going. This is a
long-term roadmap: it describes the desktop Vela wants to become, not a
schedule. Version numbers beyond 0.1 are **indicative planning targets**,
not committed scopes or promises: features can move between releases,
be split, or be dropped after design and testing. No release dates are
estimated here. Each larger piece of work starts with a design document
in `docs/`, which holds its decisions, stages and tests; this page only
links to it and says where it stands.

Vela is **alpha**, and its first beta, 0.1, is being prepared. "Done" means
implemented and tested, not validated on every GPU and driver: see
[hardware coverage](hardware.md) for the current limits.

## Beta 0.1

The first beta takes the desktop under **Done** and makes it solid, rather
than adding to it. It is Vela's first versioned release, with release notes,
alongside the per-commit alphas.

- **Feature-freeze policy:** once the 0.1 freeze is in effect, `main`
  takes only bug fixes, tests, CI, packaging, documentation and
  translations until 0.1 is released, except for the explicitly
  listed beta blockers below. The roadmap deliberately does not
  assign calendar dates to this policy or to the releases.
- **Three planned feature exceptions** may merge after the freeze:
  the Updates page ([#133](https://github.com/vela-desktop/vela/pull/133)),
  native `xdg-desktop-portal-vela`, and core Wayland text-input/input-method
  support. These are required for beta 0.1; the emoji picker is not.
- Other feature pull requests remain open for review but merge after 0.1;
  the same applies to everything under **Next**.
- GPU verification follows the
  [GPU CI policy](ci-quality.md#gpu-ci-policy), including both real-hardware
  suites on the exact beta release candidate commit.

## Beta 0.1 release blockers and validation

- **Native portal:** design and implementation of `xdg-desktop-portal-vela`
  are still pending; its backend must replace KDE/wlr for file chooser,
  screenshots, screen sharing and appearance, verified in a clean Vela session.
  The detailed portal design is intentionally a separate follow-up.
- **NVIDIA capture regression:** [#132](https://github.com/vela-desktop/vela/issues/132)
  documents synchronous Vulkan SHM readback blocking the compositor event loop
  and missing 180 Hz vblanks in the headless NVIDIA capture test. Do not
  weaken test thresholds or silently waive this known regression before release.
- **Release readiness:** passing hosted CI does not replace the required
  AMD/NVIDIA hardware runs on the release candidate; see the
  [single GPU CI policy](ci-quality.md#gpu-ci-policy).
- **Stabilization proposals (drafts, not fixes):**
  [Bluetooth pairing #146](https://github.com/vela-desktop/vela/pull/146),
  [Vulkan device loss #147](https://github.com/vela-desktop/vela/pull/147),
  [OOM contract #148](https://github.com/vela-desktop/vela/pull/148), and
  [lock wallpaper quality #149](https://github.com/vela-desktop/vela/pull/149).
  Their acceptance criteria and beta-blocker severity need review; no
  implementation is implied by opening those documentation PRs.

## Indicative releases after 0.1

These are **working hypotheses, not release commitments**. They express a
direction and a likely sequence, not deadlines. In particular, features
listed for 0.3 and the visual approach to 0.4 may change substantially.
Finishing a version means validating its defined scope, not shipping it
on a predetermined calendar.

### 0.2 — Touch and on-screen input (tentative)

Focus on making Vela practical on touchscreens and convertible devices:

- End-to-end touchscreen interaction across the compositor, shell,
  Settings and Vela apps, plus gestures and tablet-mode behavior
  ([touch design #94](https://github.com/vela-desktop/vela/pull/94)).
- A native `vela-onscreen-keyboard`, summoned by touch-focused text fields
  and integrated with the text-input/input-method foundation delivered
  in 0.1 ([OSK design #95](https://github.com/vela-desktop/vela/pull/95)).
- Pen/tablet-v2 support as described by the touch design, with its
  precise scope determined by hardware availability and testing.

The likely release criterion is a usable physical touch session with
reliable focus, keyboard show/hide behavior, rotation and multi-output
scaling. This is a **proposed** criterion, not a claim of implemented support.

### 0.3 — Desktop features and redesign groundwork (tentative)

Focus on completing selected desktop capabilities without rushing a
full visual rework:

- Candidates already under review: expanded Snipping Tool
  ([#118](https://github.com/vela-desktop/vela/pull/118)),
  screen-sharing status/privacy UI
  ([#72](https://github.com/vela-desktop/vela/pull/72)), and
  always-on-top window management
  ([#108](https://github.com/vela-desktop/vela/pull/108)).
- Selective shell, Files and Settings improvements from **Next**,
  chosen by usefulness and maintainability, not a commitment to
  implement the entire backlog.
- Prepare the future visual system: design exploration, common
  components, tokens, interaction behavior and migration strategy,
  while keeping the shipped interface coherent.

The concrete feature list is deliberately open until 0.2 is stable.

### 0.4 — Major visual redesign (highly tentative)

A possible milestone for Vela's own visual identity and a coordinated
redesign across shell, Settings, Files and first-party applications.
Explore alternative appearances (including a more Windows-like style
as an **optional** secondary choice) without making an imitation of
another desktop the default identity.

A major redesign should follow usable prototypes, coherent shared
components, accessibility/scaling checks and a manageable migration
plan. **0.4 is a placeholder, not a guarantee:** design work might
justify a different version, different scope or several smaller releases.

## In progress

| Work | Design | Pull request | Status | Release target |
|---|---|---|---|---|
| **Updates page**: Settings → System → Updates checks for a newer Vela with `vela-update` and starts the update in a terminal | in the PR | [#133](https://github.com/vela-desktop/vela/pull/133) | draft | 0.1 |
| **Vela native desktop portal**: implement `xdg-desktop-portal-vela` and replace the KDE and wlr portal backends for file chooser, screen sharing, screenshots and appearance settings | design/implementation pending | — | required release blocker; validate portal integration and remove runtime dependencies on KDE/wlr portal backends | 0.1 |
| **Text input and input methods**: Wayland text-input/input-method support for IME clients and interactive text entry | design/implementation pending | — | required for beta; integration and end-to-end input method tests to define | 0.1 |
| **Touch and pen**: touch in every app and in the shell, edge gestures, the pen over `tablet-v2`, tablet mode | in the PR | [#94](https://github.com/vela-desktop/vela/pull/94) | design under discussion | 0.2 (tentative) |
| **On-screen keyboard**: `vela-onscreen-keyboard`, opened when a text field is focused by touch | in the PR | [#95](https://github.com/vela-desktop/vela/pull/95) | design under discussion | 0.2 (tentative) |
| **Snipping tool**: text from a capture (OCR), a delay timer, a color picker and screen recording | in the PR | [#118](https://github.com/vela-desktop/vela/pull/118) | draft | 0.3 (tentative) |
| **Screen sharing indicator**: a taskbar icon while the screen is shared, with notifications held back | in the PR | [#72](https://github.com/vela-desktop/vela/pull/72) | draft | 0.3 (tentative) |
| **Always on top**, from the window menu | in the PR | [#108](https://github.com/vela-desktop/vela/pull/108) | draft | 0.3 (tentative) |

## Next

Unscheduled ideas and capabilities that are **not already implemented**,
grouped by area, in no priority order. They are not automatically promised
for 0.2, 0.3 or 0.4; moving an item into a version requires scope and
acceptance criteria. Where something exists in part, the entry names
the missing increment rather than describing all of it as absent.

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
- An emoji picker on Super+. (core text-input/input-method support targets beta 0.1).
- Extend the existing **app-only Start menu search** to files and
  Settings, and add a Recommended files section.
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
- **Advanced adaptive power policy**: gradual idle dimming before
  display-off, ambient-light sensing, and optional automatic profile
  changes by battery/AC state. Manual power profiles, idle display-off
  and lock-on-idle are already implemented.

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
- **Debian** packaging and installer support; broaden and stabilize
  the already working **experimental Ubuntu 26.10 DEB/updater** (not a
  new-from-scratch Ubuntu port).
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
  (implemented and validated on multiple real devices; the broader
  [§12 hardware matrix](brightness.md#12-tests) remains useful for coverage).
- **Desktop**: Settings, Files (with a quick preview on Space: pictures, PDFs,
  text, videos and sounds), desktop icons, virtual desktops, Task View,
  snap layouts and Snap Assist, modern context menus, light mode, a
  show-desktop strip, Caffeine, and file transfers in the taskbar.
- **[Visual coherence](visual-coherence.md)**: the same pixels, clock and
  motion across compositor, shell and apps, and shared controls for all of
  Vela's interfaces.
- **Daily comfort**: clipboard history, snipping tool, touchpad gestures,
  mouse and touchpad settings.
- **Power**: manual energy-saver/balanced/performance profiles where
  `power-profiles-daemon` offers them; screen-off idle timeout and
  lock-on-idle controls in Settings.
- **Audio**: one [native backend](audio.md) for the shell and Settings, with
  per-app inputs and outputs, levels, microphone in use, device profiles,
  codecs and sample rates.
- **Reliability and security**: compositor and shell crash recovery,
  [`vela-report`](reporting.md) for guided bug reports, `vela-update`,
  [signed packages with verified provenance](package-provenance.md) for Arch
  and Fedora, and CI with sanitizers, static analysis and real AMD and NVIDIA
  GPUs (RX 9070 XT and RTX 4070 Laptop) for GPU-affecting changes and releases.
- **Languages**: English and Italian, switched live from Settings.

## Keeping this page current

This page changes in the same pull request as the work it describes:

- A new design document adds its row to **In progress**.
- The pull request that finishes the last stage moves that work to **Done**.
- Each row in **In progress** names its intended release, explicitly
  **tentative** for 0.2 and later.
- An idea from **Next** receives design and acceptance criteria before
  it moves into a release target.
- Already-shipped capabilities move to **Done**; partially shipped
  items stay in **Next** only for their missing part.
- Revisit the 0.2–0.4 proposals as feedback and engineering
  feasibility evolve, without inventing calendar deadlines.

To propose a feature, open an issue that describes the behavior first
(see [CONTRIBUTING](../CONTRIBUTING.md)).
