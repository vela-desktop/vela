# Roadmap

Where Vela is now, which capabilities are next, and how the desktop may
evolve. This is **a direction, not a calendar**: there are no release dates.
The 0.1 history is fixed. The **0.2 plan now reflects major work already
merged into main** plus a small list of possible finishing features.
Targets for **0.3–0.6 remain estimates** and can change with design,
validation and feedback. There is no committed release calendar.

Vela **0.1 is the first beta**, released as
[`v0.1.0`](https://github.com/vela-desktop/vela/releases/tag/v0.1.0) on
31 October 2026. "Done" means implemented on **main**, not
necessarily available in the latest tagged Beta or validated on every GPU
and driver: see [hardware coverage](hardware.md) for current limits. Larger
changes get their own design documents; the roadmap records intent rather
than replacing those specifications.

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

**Release of `v0.1.0` (31 October 2026):**

- **GPU and session verification:** full AMD and NVIDIA suites on the
  exact release commit, as the [GPU CI policy](ci-quality.md#gpu-ci-policy)
  requires, plus physical-session smoke tests.
- **Updates in a real session:** a real terminal and "Sign out to finish"
  ([updates §11](updates.md#11-stages)); the Beta check finds `v0.1.0`.
- **Release rehearsal:** the whole tag pipeline on a public fork first, as
  [release channels](release-channels.md#rollout-validation) require. It
  found two release-only pipeline bugs, fixed before the real tag
  ([#177](https://github.com/vela-desktop/vela/pull/177)).

**Feature freeze:** 0.1.x remains a maintenance series, without new
user-facing features. The
[0.1.2 tracker #225](https://github.com/vela-desktop/vela/issues/225)
is separate from the next minor release. The
[OOM policy #148](https://github.com/vela-desktop/vela/pull/148) has
already merged into main as post-0.1 work, not an outstanding beta blocker.

## 0.2 — Personalization, productivity and architectural hardening

Vela 0.2 need not wait for touch, pen or the on-screen keyboard.
Those projects are substantial and now provisionally target 0.3.
Distinguish **merged on main**, **candidate for 0.2**, and
**shipped in a tagged Beta**: these are not interchangeable statuses.

**Already merged into main since the 0.1 branch split:**

- **Theme engine and personalization:** declarative themes, per-surface
  styles, Window Glass and Vela Floating
  ([#207](https://github.com/vela-desktop/vela/pull/207)); redesigned
  personalization, one-click theme galleries, previews and Undo
  ([#212](https://github.com/vela-desktop/vela/pull/212)); accent,
  clock, taskbar and Start preferences
  ([#216](https://github.com/vela-desktop/vela/pull/216)); expressive
  controls ([#244](https://github.com/vela-desktop/vela/pull/244)) and
  Vela Expressive with wallpaper-derived dynamic color, motion and
  Start/Quick Settings/notification variants
  ([#248](https://github.com/vela-desktop/vela/pull/248)).
- **Daily desktop:** battery notifications for computers and peripherals
  ([#215](https://github.com/vela-desktop/vela/pull/215));
  screen-sharing indicator, capture attribution and automatic
  Do Not Disturb ([#220](https://github.com/vela-desktop/vela/pull/220));
  right-click notification dismissal
  ([#188](https://github.com/vela-desktop/vela/pull/188));
  a taskbar that pages its app buttons when space runs out
  ([#219](https://github.com/vela-desktop/vela/issues/219),
  [#266](https://github.com/vela-desktop/vela/pull/266)).
  Expressive already implements a grouped-notification variant.
- **Integration and compatibility:** update-check scheduling
  ([#190](https://github.com/vela-desktop/vela/pull/190));
  showing packaging recipes during updates
  ([#247](https://github.com/vela-desktop/vela/pull/247));
  Fedora 45 packaging ([#224](https://github.com/vela-desktop/vela/pull/224));
  a temporary choice of secret backend pending Vela Secrets
  ([#222](https://github.com/vela-desktop/vela/issues/222),
  [#255](https://github.com/vela-desktop/vela/pull/255)) and KWallet's Secret Service
  started through `ksecretd`
  ([#277](https://github.com/vela-desktop/vela/pull/277));
  and experimental Vulkan Venus virtual-machine support
  ([#251](https://github.com/vela-desktop/vela/pull/251)).
  Experimental capabilities still need hardware validation.
- **Core improvements:** host allocation and OOM handling, renderer
  pass reuse and bounded GPU/icon memory
  ([#148](https://github.com/vela-desktop/vela/pull/148),
  [#243](https://github.com/vela-desktop/vela/pull/243),
  [#246](https://github.com/vela-desktop/vela/pull/246),
  [#250](https://github.com/vela-desktop/vela/pull/250),
  [#252](https://github.com/vela-desktop/vela/pull/252));
  compositor security hardening
  ([#258](https://github.com/vela-desktop/vela/pull/258));
  Settings that keep the saved preference when a write fails
  ([#218](https://github.com/vela-desktop/vela/issues/218),
  [#267](https://github.com/vela-desktop/vela/pull/267));
  x86-64-v3 builds ([#245](https://github.com/vela-desktop/vela/pull/245));
  and experimental Intel ANV GPU CI
  ([#226](https://github.com/vela-desktop/vela/pull/226)).

**Candidates to evaluate before 0.2, not automatically release blockers:**

- Finish or deliberately defer the existing
  [Snipping Tool #118](https://github.com/vela-desktop/vela/pull/118)
  (OCR, timer, color picker, recording) and
  [always-on-top #108](https://github.com/vela-desktop/vela/pull/108).
  Both are older draft PRs requiring compatibility work, review and tests;
  do not merge them solely to satisfy the roadmap.
- Evaluate taskbar app shortcuts (Super+1 through Super+9) and configurable
  initial NumLock state requested in
  [community feedback #214](https://github.com/vela-desktop/vela/issues/214).
- Finish the desktop configuration fix
  [#235](https://github.com/vela-desktop/vela/pull/235) /
  [#236](https://github.com/vela-desktop/vela/issues/236).
- Optionally evaluate
  [nested Distrobox #203](https://github.com/vela-desktop/vela/pull/203)
  and, from the [backlog](#next--not-yet-assigned-to-a-release), taskbar
  auto-hide and Start file/settings search, only if bounded and well
  tested.

**Release gate:** decide the final shortlist, freeze user-facing work and
validate the exact candidate commit with required AMD/NVIDIA GPU suites,
real Wayland login sessions, security/lock/crash recovery, keyrings,
themes, packaging and updater tests. Investigate
[Ubuntu GPU failures #197](https://github.com/vela-desktop/vela/issues/197).
Follow the signed-tag rehearsal and
[release process](release-channels.md). There is **no fixed ship date**;
0.1.x backports remain independent.

## 0.3 — Touch, pen, input methods and on-screen keyboard (indicative)

This moves the original 0.2 input work here as a **proposed** feature
scope; none of these features is implied to be implemented yet:

- **Wayland text input and IMEs:** text-input-v3, input-method-v2,
  focus and preedit/commit correctness, real fcitx5/IBus interoperability.
  Reconcile the input-method stages with the OSK design.
- **Touch, pressure-sensitive pen and tablet mode:**
  [design #94](https://github.com/vela-desktop/vela/pull/94), stages
  T0–T5 (native wl_touch, pointer compatibility including X11, gestures,
  decorations and hover-free UI), P1–P2 (tablet-v2 pressure, tilt, eraser,
  buttons and curves) and M1 (tablet mode and sensor rotation).
  Mapping multiple external touchscreens to outputs remains explicitly
  outside the initial design unless separately scoped.
- **Native on-screen keyboard:**
  [design #95](https://github.com/vela-desktop/vela/pull/95), K0–K5,
  with hardware layouts, manual and touch-triggered invocation,
  docked/floating and specialized layouts, variants/emoji/clipboard,
  secure input and touch-only lock/polkit access.

**Validation:** real touchscreen, pen and convertible/tablet testing;
rotation/scaling and gestures, touch-only unlock and password entry,
IME/Xwayland interoperability; no regressions for existing desktop
input, compositor or GPU behavior.

Further everyday-desktop features need an explicit scope decision.
The former 0.3 completeness wishlist is preserved as an **unassigned
backlog** below, not silently committed to this release.

## 0.4 — Coordinated visual redesign (highly indicative)

Possible milestone for a distinctive, coherent default Vela visual language
across compositor-owned chrome, shell, Settings, Files and first-party
tools. The theme system and Vela Expressive already in main are
not themselves the coordinated default redesign. Candidate supporting UX
work: Start's Recommended content and pinned-app folders, taskbar layouts
(including vertical and uncombined buttons), app badges, system sounds,
font/regional settings and printing/scanning integration. Which of these
belongs in 0.4 is provisional.

**Design development is intentionally independent of release integration.**
A dedicated long-lived branch may be used for gradual exploration and
prototyping **even before 0.2 ships**. Early exploration does not by itself
justify merging an unfinished cross-desktop redesign into 0.2 or 0.3.
Validate reusable components, scaling, accessibility and migration before
proposing an integration series.

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
| **Snipping Tool extensions**: OCR, delay, color picker and recording | in PR | [#118](https://github.com/vela-desktop/vela/pull/118) | draft; rebase, review and test | 0.2 candidate |
| **Always-on-top** | in PR | [#108](https://github.com/vela-desktop/vela/pull/108) | draft; review and test | 0.2 candidate |
| **Desktop preferences** | [#236](https://github.com/vela-desktop/vela/issues/236) | [#235](https://github.com/vela-desktop/vela/pull/235) | open fix PR | 0.2 stabilization candidate |
| **Text input and IME** | reconcile with OSK | — | not implemented | 0.3 proposed |
| **Touch, pen and tablet mode** | [#94](https://github.com/vela-desktop/vela/pull/94) | #94 (design) | design open, code pending | 0.3 proposed |
| **On-screen keyboard** | [#95](https://github.com/vela-desktop/vela/pull/95) | #95 (design) | design open, code pending | 0.3 proposed |
| **Benchmark suite** | in PR | [#109](https://github.com/vela-desktop/vela/pull/109) | work/review ongoing | unassigned |

## Next — Not yet assigned to a release

This unscheduled backlog includes the previous 0.3 everyday-completeness
candidates. Do not treat them as promises for the new input-focused 0.3.
A row may also be named as a candidate in a release section above; it stays
here until it is merged.

| Candidate | Status |
|---|---|
| Native xdg-desktop-portal-vela backend where beneficial | Design separately; retain GTK for generic dialogs and xdg-desktop-portal-wlr for screencast/screenshots until a tested replacement exists |
| Window rules, remembering position/size, cross-monitor/desktop shortcuts and picture-in-picture | Unscheduled |
| Super+P projection menu, mirroring and improved VRR/animation policy | Unscheduled; physical display validation required |
| External touchscreen-to-output mapping beyond initial #94 scope | Unscheduled |
| Start file/settings search and taskbar auto-hide | Unscheduled; optional 0.2 candidates |
| Emoji picker, keyboard backlight, brightness key OSD and shortcut editor | Unscheduled |
| Fullscreen-aware DND and broader grouped notifications beyond Expressive | Unscheduled; screen-sharing DND and Expressive grouping already implemented |
| Screenshot annotations, Files batch rename, post-crash notification leading to vela-report | Unscheduled |
| Further Ubuntu updater and package integration validation | Continuous |
| More Wayland protocols (fifo, commit-timing, xdg-foreign, xdg-toplevel-icon, xdg-dialog) | Evaluate individually; security-context support already merged via #258 |
| Live blur behind compositor title bars and caching unchanged blurred backgrounds | Unscheduled |
| Official pacman package repository for system-managed updates | Unscheduled |
| Additional languages and community translation workflow | Continuous |

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

**Done means integrated in main, not necessarily published in 0.1 Beta.**
Notable post-0.1 work includes the theme/personalization system
([#207](https://github.com/vela-desktop/vela/pull/207),
[#212](https://github.com/vela-desktop/vela/pull/212),
[#216](https://github.com/vela-desktop/vela/pull/216),
[#248](https://github.com/vela-desktop/vela/pull/248)),
battery notifications ([#215](https://github.com/vela-desktop/vela/pull/215)),
screen-sharing indicator ([#220](https://github.com/vela-desktop/vela/pull/220)),
update scheduling ([#190](https://github.com/vela-desktop/vela/pull/190)),
memory policy and security hardening described under 0.2.

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
  GPUs (self-hosted RX 9070 XT and per-run Vast.ai NVIDIA RTX rentals) for GPU-affecting changes and releases.
- **Languages**: English and Italian, switched live from Settings.

## Keeping this page current

This page changes in the same pull request as the work it describes:

- A new design document adds its row to **In progress**.
- The pull request that finishes the last stage moves that work to **Done**.
- Keep 0.1.x maintenance and 0.2 release preparations separate.
- Distinguish merged main changes, proposed release candidates and
  actually published Beta features.
- Finalize the remaining 0.2 scope explicitly; review and either accept
  or defer old draft PRs, never merge them unconditionally.
- Mark all unreleased targets as estimates and revise them after testing.
- Work on a future redesign branch or independent font/icon repositories
  may begin early without merging unfinished designs into `main`.
- When a capability is merged into `main`, move it to **Done** and keep only
  its missing increment in **Next**. Never re-list implemented features
  such as clipboard history as future work.

To propose a feature, open an issue that describes the behavior first
(see [CONTRIBUTING](../CONTRIBUTING.md)).
