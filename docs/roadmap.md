# Roadmap

Where Vela is now, which capabilities are next, and how the desktop may
evolve. This is **a direction, not a calendar**: there are no release dates.
The 0.1 and 0.2 scopes below reflect current project decisions; release
targets for **0.3–0.6 are estimates** that can change after design, testing,
and feedback. Implementation work can start before its target release
without merging into `main`.

Vela **0.1 is the first beta**. "Done" means
implemented and tested, not validated on every GPU and driver: see
[hardware coverage](hardware.md) for current limits. Larger changes get
their own design documents; the roadmap records intent rather than replacing
those specifications.

## 0.1 — First beta: stability, integration, release readiness

The beta stabilizes and packages what Vela already provides, removes unwanted
desktop dependencies, and publishes a versioned, signed release. Work landing
during the 0.1 feature freeze is restricted to fixes, tests, CI, packaging,
release engineering, documentation and translations; the Updates page was
the only feature exception.

**Done for 0.1:**

- **Updates page:** Settings shows the installed and newest version, checks
  for updates, shows what's new and errors, and launches the updater in a
  terminal; Vela checks in the background and sends a notification
  ([#161](https://github.com/vela-desktop/vela/pull/161), which superseded
  [#133](https://github.com/vela-desktop/vela/pull/133);
  [design](updates.md)). Real updates were verified in clean Arch, Fedora 44
  and Ubuntu 26.10 containers.
- **NVIDIA capture performance:**
  [#132](https://github.com/vela-desktop/vela/issues/132) is fixed by
  deferred SHM readback that no longer blocks presentation
  ([#156](https://github.com/vela-desktop/vela/pull/156),
  [#173](https://github.com/vela-desktop/vela/pull/173)), without
  relaxing the tests.
- **Native Bluetooth pairing and device lifecycle:** the session
  `vela-bluetooth` daemon with its BlueZ pairing agent, confirmation,
  PIN and passkey questions, and the shell, quick settings and Settings on
  top of it ([#146](https://github.com/vela-desktop/vela/pull/146),
  [#157](https://github.com/vela-desktop/vela/pull/157);
  [design](bluetooth.md)).
- **Vela Lock wallpaper quality:** a sharp blurred wallpaper on the
  password screen ([#149](https://github.com/vela-desktop/vela/pull/149);
  [notes](lock-wallpaper-quality.md)).
- **Vulkan device loss:** the compositor detects a lost device, stops GPU
  work and exits so the supervisor can restart it
  ([#147](https://github.com/vela-desktop/vela/pull/147);
  [policy](vulkan-device-loss.md)).
- **Packaging and runtime independence:** clean installation with the GTK
  portal for generic dialogs and `xdg-desktop-portal-wlr` for screencast
  and screenshots, without `xdg-desktop-portal-kde` or Plasma Workspace
  ([#120](https://github.com/vela-desktop/vela/pull/120)).
- **Release channels:** Beta (`vela`, signed `v0.x.y` releases built from
  a protected tag) and Experimental (`vela-git`, development commits) on
  Arch, Fedora 44 and Ubuntu 26.10
  ([#160](https://github.com/vela-desktop/vela/pull/160);
  [release channels](release-channels.md)).
- **X11 at native resolution:** X11 apps and fullscreen games run 1:1 in
  the output's pixels, with direct scanout
  ([#167](https://github.com/vela-desktop/vela/pull/167)).

**Before tagging `v0.1.0`:**

- **GPU and session verification:** follow the
  [GPU CI policy](ci-quality.md#gpu-ci-policy), including full AMD and
  NVIDIA suites on the exact release commit, plus physical-session smoke
  tests.
- **Updates in a real session:** a real terminal and "Sign out to finish"
  ([updates §11](updates.md#11-stages)); Beta updates are checked again once
  `v0.1.0` exists.
- **Release rehearsal:** a versioned test tag through the whole release
  workflow, as [release channels](release-channels.md#rollout-validation)
  require.

**Feature-freeze rule:** no new user-facing features are added to 0.1. Core
Wayland `text-input-v3`/`input-method-v2` and on-screen keyboard work target
0.2 instead. Bug fixes, reliability work and necessary packaging are not
reclassified as new features.

**Stabilization proposal still open:**
[OOM contract #148](https://github.com/vela-desktop/vela/pull/148). Whether
it blocks a release depends on review and reproducible severity, not on the
existence of a design draft.

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

**Validation:** physical touchscreen, pressure-sensitive pen and
convertible/tablet tests; rotation/scaling and gesture regression tests;
a touch-only lock/unlock and password-entry scenario; real IME and
Xwayland client interoperability. Existing keyboard, mouse, compositor
and GPU behavior must not regress.

## 0.3 — Everyday desktop completeness (indicative)

A focused feature release that closes user-facing gaps without a
large redesign or renderer overhaul. Candidates:

- **Native `xdg-desktop-portal-vela` backend:** design, implement and test
  Vela's own portal integration as a 0.3 target, replacing the generic GTK
  backend where it offers a benefit. Retain `xdg-desktop-portal-wlr` for
  screenshots and screen sharing; its replacement is not part of this work.

- Better window management: app-specific window rules, remembering
  window position/size, moving windows across outputs/desktops by
  shortcut, and defined picture-in-picture behavior.
- Displays and compatibility: projection menu (Super+P), mirroring,
  refined VRR/animation policy,
  and external touchscreen/output mapping that was explicitly
  outside the initial touch design.
- Shell and Settings: Start search extended beyond existing app search
  to files/settings, a global emoji picker, taskbar auto-hide,
  grouped notifications, fullscreen-aware Do Not Disturb, brightness
  key OSD and keyboard backlight controls, and a shortcut editor.
- Incremental tools: screenshot annotations, batch rename in Files,
  and a post-crash
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
| **Release of `v0.1.0`**: GPU suites on the release commit, a real-session Updates check, a rehearsal with a test tag | [Release channels](release-channels.md) | [#160](https://github.com/vela-desktop/vela/pull/160) (pipeline, merged) | pipeline merged; verification and the tag pending | 0.1 |
| **Host OOM allocation contract** | in the PR | [#148](https://github.com/vela-desktop/vela/pull/148) | draft; release classification after review | to be decided |
| **Text input and input methods**: `text-input-v3`, `input-method-v2`, IME integration | reconcile with OSK design | — | not implemented | 0.2 |
| **Touch, pressure-sensitive pen and tablet mode**: full agreed #94 stages T0–T5, P1–P2 and M1 | [#94](https://github.com/vela-desktop/vela/pull/94) | #94 (design) | design under discussion; implementation pending | 0.2 |
| **On-screen keyboard**: full #95 K0–K5, including touch-only unlocking | [#95](https://github.com/vela-desktop/vela/pull/95) | #95 (design) | design under discussion; implementation pending | 0.2 |
| **Snipping Tool extensions**: OCR, delay, color picker and recording | in the PR | [#118](https://github.com/vela-desktop/vela/pull/118) | draft; requires review and tests | 0.2 |
| **Screen-sharing indicator and notification protection** | in the PR | [#72](https://github.com/vela-desktop/vela/pull/72) | draft; requires review and tests | 0.2 |
| **Always-on-top** | in the PR | [#108](https://github.com/vela-desktop/vela/pull/108) | draft; requires review and tests | 0.2 |

## Next — Not yet assigned to a release

Only work **not already described in the 0.1–0.6 release sections**
belongs here; those sections are the single reference for their respective
release candidates. This is an unscheduled backlog, not a commitment.

| Candidate | Status |
|---|---|
| More Wayland protocols (`fifo`, `commit-timing`, `security-context`, `xdg-foreign`, `xdg-toplevel-icon`, `xdg-dialog`) | Evaluate individually when needed |
| Live blur behind compositor-drawn title bars and caching of unchanged blurred backgrounds | Unscheduled; evaluate alongside visual work |
| An official pacman package repository for system-managed updates | Unscheduled |
| More languages and a community translation workflow | Continuous; no fixed release |

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
- **Updates**: the [Updates page](updates.md) in Settings, background checks
  and notifications, over `vela-update-daemon`; Beta and Experimental
  [release channels](release-channels.md) on Arch, Fedora 44 and Ubuntu 26.10.
- **Compositor and renderer**: a [C core](c-core.md), Vela's own scene and
  [Vulkan renderer](renderer.md), pixel-exact fractional scaling, late
  latching, live acrylic blur, rounded corners and shadows, Vela's title bar,
  and a clean exit on [Vulkan device loss](vulkan-device-loss.md).
- **Displays and gaming**: native display configuration with compositor-owned
  rollback, multi-monitor hotplug with per-output scale, direct scanout, VRR,
  tearing control, explicit sync, X11 apps and games at native resolution,
  Night Light, and
  [brightness](brightness.md) per monitor over DDC/CI and the kernel backlight
  (implemented and tested, including on real hardware).
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
- **Bluetooth**: the [`vela-bluetooth`](bluetooth.md) daemon with its own
  pairing agent, used by the shell, quick settings, Settings and audio.
- **Reliability and security**: compositor and shell crash recovery,
  [`vela-report`](reporting.md) for guided bug reports, `vela-update`,
  [signed packages with verified provenance](package-provenance.md) for Arch
  and Fedora and for every Beta release, and CI with sanitizers, static analysis and real AMD and NVIDIA
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
