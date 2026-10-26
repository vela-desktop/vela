# Roadmap

Where Vela is now, which capabilities are next, and how the desktop may
evolve. This is **a direction, not a calendar**: there are no release dates.
The 0.1 and 0.2 scopes below reflect current project decisions; release
targets for **0.3–0.6 are estimates** that can change after design, testing,
and feedback. Implementation work can start before its target release
without merging into `main`.

Vela is **alpha**, and its first beta, 0.1, is being prepared. "Done" means
implemented and tested, not validated on every GPU and driver: see
[hardware coverage](hardware.md) for current limits. Larger changes get
their own design documents; the roadmap records intent rather than replacing
those specifications.

## 0.1 — First beta: stability, integration, release readiness

The beta is primarily about stabilizing and packaging what Vela already
provides, removing unwanted desktop dependencies, and publishing a
versioned release. Work landing during the 0.1 feature freeze is restricted
to fixes, tests, CI, packaging, release engineering, documentation,
translations, and the **two named user-facing feature exceptions** below.

**Required for the 0.1 release:**

- **Native desktop portal:** design, implement and validate
  `xdg-desktop-portal-vela` for file chooser, screenshot, screencast/screen
  sharing and appearance; replace the KDE/wlr portal backends without
  retaining their runtime dependency for these functions. The portal
  architecture/design is pending a dedicated proposal.
- **Updates page:** complete
  [#133](https://github.com/vela-desktop/vela/pull/133) so Settings can
  inspect the installed and available revision, check for updates, display
  changes/errors and launch the updater. **This is a release blocker,
  not a feature that may slip to 0.2.** Automatic polling/notifications
  remain a later improvement.
- **NVIDIA capture performance:** fix and verify
  [#132](https://github.com/vela-desktop/vela/issues/132) (blocking
  synchronous SHM readback and missed 180 Hz virtual vblanks); do not
  waive the regression by relaxing the tests.
- **GPU and session verification:** follow the
  [GPU CI policy](ci-quality.md#gpu-ci-policy), including full AMD and
  NVIDIA suites on the exact release candidate; complement headless
  coverage with practical physical-session smoke tests.
- **Packaging and runtime independence:** clean installation and
  correct portal/runtime dependencies, preserving an independent Vela
  compositor and shell. Review the open
  [packaging #120](https://github.com/vela-desktop/vela/pull/120)
  against the final portal configuration.

**Release engineering (0.1 infrastructure, not a desktop feature):**
design and build a more mature release process for the first beta:
versioned/tagged releases and release notes, reproducible artifact
identification, CI/release-candidate verification, provenance/signing,
publication and rollback/update-channel expectations. Reuse existing
`vela-update` and provenance machinery; do not describe these capabilities
as absent merely because the release process needs to mature. The exact
workflow remains to be designed, separately from feature planning.

**Feature-freeze rule:** no new user-facing features besides the native
portal and Updates page are added to 0.1. Core Wayland
`text-input-v3`/`input-method-v2` and on-screen keyboard work target
0.2 instead. Existing bug fixes, reliability work and necessary packaging
are not reclassified as new features.

**Stabilization proposals, not shipped fixes:**
[Bluetooth pairing #146](https://github.com/vela-desktop/vela/pull/146),
[Vulkan device loss #147](https://github.com/vela-desktop/vela/pull/147),
[OOM contract #148](https://github.com/vela-desktop/vela/pull/148) and
[Vela Lock wallpaper quality #149](https://github.com/vela-desktop/vela/pull/149).
Their final release-blocker classification depends on review and
reproducible severity, not just on the existence of a design draft.

## 0.2 — Touch, pen, input methods and completion of existing feature PRs

This version has a **defined feature scope**, not a tentative list of new
ideas. Its two large projects are the complete touch/pen design and a
native on-screen keyboard, including their Wayland input foundations.

- **Wayland input foundations:** `text-input-v3` and `input-method-v2`;
  proper focus, preedit/commit, text input and input-method integration,
  with real IME test cases (e.g. fcitx5/IBus). This work is **in 0.2,
  not 0.1**.
- **Complete [touch/pen design #94](https://github.com/vela-desktop/vela/pull/94):**
  T0–T5 (native `wl_touch`, emulated pointer where needed including X11,
  touchscreen gestures, decorations, shell and app interactions without
  hover), P1–P2 (pen over `tablet-v2` with pressure, tilt, eraser,
  buttons and pressure curve, plus touchpad use where supported), and M1
  (tablet mode and sensor-based automatic rotation). Follow the design's
  explicitly stated hardware limitations: mapping multiple external
  touchscreens to outputs is **not** silently included.
- **Complete [on-screen keyboard design #95](https://github.com/vela-desktop/vela/pull/95):**
  K0–K5 for a native Qt Quick keyboard with hardware layouts, manual
  and touch-triggered display, docked/floating positions, specialized
  layouts, variants/emoji/clipboard panels, lock/polkit access and
  safe input delivery. Reconcile the design's K0/I1 split with the
  decision that input-method support is delivered in 0.2.
- **Resolve older open feature PRs:** expanded Snipping Tool
  [#118](https://github.com/vela-desktop/vela/pull/118) (OCR, timer,
  color picker and recording), screen-sharing indicator/privacy behavior
  [#72](https://github.com/vela-desktop/vela/pull/72), and always-on-top
  [#108](https://github.com/vela-desktop/vela/pull/108). Each requires
  review, compatibility work and tests; **merge when ready or close with
  a documented reason if superseded/rejected**. "Resolve" does not mean
  blindly merging every draft.
- Finish or explicitly supersede the related touch/keyboard design PRs
  once their accepted design is integrated into the implementation.
  The Updates page #133 must already be finished for 0.1 and is not
  part of this deferred-feature list.

**Validation:** physical touchscreen, pressure-sensitive pen and
convertible/tablet tests; rotation/scaling and gesture regression tests;
a touch-only lock/unlock and password-entry scenario; real IME and
Xwayland client interoperability. Existing keyboard, mouse, compositor
and GPU behavior must not regress.

## 0.3 — Everyday desktop completeness (indicative)

A focused feature release that closes user-facing gaps without a
large redesign or renderer overhaul. Candidates:

- Better window management: app-specific window rules, remembering
  window position/size, moving windows across outputs/desktops by
  shortcut, and defined picture-in-picture behavior.
- Displays and compatibility: projection menu (Super+P), mirroring,
  self-scaling X11 application option, refined VRR/animation policy,
  and external touchscreen/output mapping that was explicitly
  outside the initial touch design.
- Shell and Settings: Start search extended beyond existing app search
  to files/settings, a global emoji picker, taskbar auto-hide,
  grouped notifications, fullscreen-aware Do Not Disturb, brightness
  key OSD and keyboard backlight controls, and a shortcut editor.
- Incremental tools: screenshot annotations, batch rename in Files,
  opt-in automatic update checks/notifications, and a post-crash
  notification leading to the already implemented `vela-report`.
- Broaden and physically validate the experimental Ubuntu updater
  and package integration.

The items above are **suggestions, not obligations**; refine based on
actual 0.2 results and user impact. A standalone full content indexer
is not implied by extending Start search.

## 0.4 — Coordinated visual redesign (highly indicative)

Possible milestone for a distinctive, coherent Vela visual language
across compositor-owned chrome, shell, Settings, Files and first-party
tools. Candidate supporting UX work: Start's Recommended content and
pinned-app folders, taskbar layouts (including vertical and uncombined
buttons), app badges, system sounds, font/regional settings and
printing/scanning integration. Which of these belongs in 0.4 is
provisional.

**Design development is intentionally independent of release integration.**
A dedicated long-lived branch may be used for gradual exploration and
prototyping **even before 0.2 ships**. It must not merge visual redesign
changes into `main` during the 0.1/0.2/0.3 scopes merely because the
experiments began early. Validate reusable components, scaling,
accessibility and migration before proposing an integration series.

Type and icon work may live in **separate dedicated repositories**
(for a Vela font and an icon system, respectively), allowing independent
iteration, licensing, versioning and tests. Those repositories and their
interfaces are **future possibilities, not already created projects**;
decisions on ownership and integration stay open. Alternative visual
styles (including a more Windows-like secondary option) are exploration
ideas, not a commitment to imitate another desktop.

## 0.5 — Hardware, graphics and platform depth (indicative)

Candidate larger systems work:

- HDR and 10-bit output support, `color-management-v1`, per-display ICC
  profiles and clearly defined color/brightness behavior.
- Hybrid/multi-GPU rendering and device selection, plus optional
  per-monitor virtual desktops where the architecture permits.
- Battery health/charge thresholds, ambient-light and adaptive
  power policy, users/accounts, calendar-event integration.
- A shared file/content indexing service, Files remote locations
  (if not completed earlier), disk-image mounting and more complete
  physical-device integration where appropriate.
- A native Task Manager, a standalone image viewer if justified,
  Debian packaging and a practical live image or VM distribution.

These are substantial independent projects; do not treat them as one
all-or-nothing compatibility promise.

## 0.6 — Ecosystem and longer-horizon completeness (indicative)

Potential future work includes full session/app restore, widgets and
their data sources, file tagging, deeper system/application services,
optional audio ducking, and **only if justified** additional first-party
apps such as a text editor, terminal or calculator. Each would need its
own design, cost/maintenance analysis and definition of done.

## In progress

| Work | Design | Pull request | Status | Release target |
|---|---|---|---|---|
| **Settings Updates page**: installed/latest revision, check, changes and update launch | proposed in the PR | [#133](https://github.com/vela-desktop/vela/pull/133) | draft; **0.1 release blocker** | 0.1 |
| **Native desktop portal**: replace KDE/wlr backends for chooser, screenshots, screencast and appearance | design pending | — | **0.1 release blocker** | 0.1 |
| **Release engineering**: versioned beta pipeline, gates, provenance, artifacts and release notes | separate infrastructure design pending | — | release infrastructure, **not a desktop feature** | 0.1 |
| **Text input and input methods**: `text-input-v3`, `input-method-v2`, IME integration | reconcile with OSK design | — | not implemented | 0.2 |
| **Touch, pressure-sensitive pen and tablet mode**: full agreed #94 stages T0–T5, P1–P2 and M1 | [#94](https://github.com/vela-desktop/vela/pull/94) | #94 (design) | design under discussion; implementation pending | 0.2 |
| **On-screen keyboard**: full #95 K0–K5, including touch-only unlocking | [#95](https://github.com/vela-desktop/vela/pull/95) | #95 (design) | design under discussion; implementation pending | 0.2 |
| **Snipping Tool extensions**: OCR, delay, color picker and recording | in the PR | [#118](https://github.com/vela-desktop/vela/pull/118) | draft; requires review and tests | 0.2 |
| **Screen-sharing indicator and notification protection** | in the PR | [#72](https://github.com/vela-desktop/vela/pull/72) | draft; requires review and tests | 0.2 |
| **Always-on-top** | in the PR | [#108](https://github.com/vela-desktop/vela/pull/108) | draft; requires review and tests | 0.2 |

## Next

The following is a **feature inventory, not additional release commitments**.
A suggested version is given to make the ideas comparable; 0.3–0.6 targets
can change. Features already delivered are in **Done**. Where an existing
feature is partial, the inventory names only its missing extension.

### Graphics and displays

| Candidate | Suggested release |
|---|---|
| Refine VRR behavior during animations | 0.3 |
| Allow X11 apps to render at their own fractional scale | 0.3 |
| Super+P projection choices and output mirroring | 0.3 |
| External touchscreen mapping beyond the scope of #94 | 0.3 |
| Live blur behind Vela's compositor-drawn title bar and blur caching | 0.4 |
| HDR, 10-bit outputs and `color-management-v1` ([renderer](renderer.md#75-color)) | 0.5 |
| Per-output ICC profiles and HDR brightness semantics ([#45](https://github.com/vela-desktop/vela/pull/45)) | 0.5 |
| Hybrid graphics, multi-GPU and app GPU selection | 0.5 |
| More Wayland protocols (`fifo`, `commit-timing`, `security-context`, `xdg-foreign`, `xdg-toplevel-icon`, `xdg-dialog`) | When justified; evaluate individually |

### Windows and multitasking

| Candidate | Suggested release |
|---|---|
| App-specific window rules | 0.3 |
| Remember window position and size (not full application restart) | 0.3 |
| Keyboard shortcuts to move windows across outputs/desktops | 0.3 |
| Picture-in-picture policy and UI | 0.3 |
| Optional per-monitor virtual desktops | 0.5 |
| Restore applications and session state after login | 0.6 |

### Shell and interaction

| Candidate | Suggested release |
|---|---|
| Global emoji picker (not the on-screen keyboard's emoji panel) | 0.3 |
| Extend existing app search in Start to files/Settings | 0.3 |
| Taskbar auto-hide | 0.3 |
| Group notifications; fullscreen-aware Do Not Disturb | 0.3 |
| Brightness hardware keys with an OSD; keyboard backlight control | 0.3 |
| Start Recommended files and pinned-app folders | 0.4 |
| Generic app taskbar progress/badges (Vela Files transfers are already covered) | 0.4 |
| Vertical taskbar and independent/uncombined taskbar buttons | 0.4 |
| Calendar events in the notification center (calendar UI already exists) | 0.5 |
| Widgets: weather, calendar, news, system status | 0.6 |

### Snipping Tool and Files

| Candidate | Suggested release |
|---|---|
| Screenshot annotations (pen, highlighter, arrows, crop) | 0.3 |
| Files batch rename | 0.3 |
| Files remote SMB/SFTP/WebDAV locations | 0.4 |
| Phones and cameras via MTP/PTP | 0.4 |
| Shared indexed file/content search | 0.5 |
| Disk-image mounting | 0.5 |
| File tags | 0.6 |

### Settings, services and audio

| Candidate | Suggested release |
|---|---|
| Automatic update checks/notifications (manual Updates page is a 0.1 blocker) | 0.3 |
| Keyboard shortcut editor | 0.3 |
| Post-crash notice linking to the existing `vela-report` | 0.3 |
| Font choices and regional number/date formats | 0.4 |
| Printers and scanners | 0.4 |
| System sounds and separate volume | 0.4 |
| Users, accounts, login configuration | 0.5 |
| Battery health and hardware charge limits | 0.5 |
| Adaptive power policy: gradual idle dimming, ambient light, automatic profiles | 0.5 |
| Calendar-event integration | 0.5 |
| Optional audio ducking during calls ([audio design](audio.md#10-not-in-scope)) | 0.6 if adopted |

### Vela applications, languages and distribution

| Candidate | Suggested release |
|---|---|
| Extend/validate existing experimental Ubuntu 26.10 DEB/updater | 0.3 |
| More languages and a community translation workflow | Continuous from 0.3 |
| Official pacman package repository | 0.4 |
| Native Task Manager (processes, CPU/GPU/memory/disk/network/startup) | 0.5 |
| Standalone image viewer beyond Files quick preview | 0.5 if justified |
| Debian packaging and installer | 0.5 |
| Live installer/demo image or supported VM image | 0.5 |
| Own editor, terminal and calculator | 0.6 only if worthwhile |

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
- Keep **0.1 release blockers** separate from the 0.1 release-engineering
  workstream and from 0.2 input feature development.
- Respect the agreed **0.2 scope**; finishing old feature PRs means
  reviewing and resolving them, not merging drafts unconditionally.
- Mark 0.3–0.6 scope and version assignments as estimates, and revise
  as designs and testing evolve.
- Work on a future redesign branch or independent font/icon repositories
  may begin early without merging unfinished designs into `main`.
- When a capability actually ships, move it to **Done** and keep only
  its missing increment in **Next**. Never re-list implemented features
  such as clipboard history as future work.

To propose a feature, open an issue that describes the behavior first
(see [CONTRIBUTING](../CONTRIBUTING.md)).
