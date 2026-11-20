# Roadmap

The current Beta is
[`v0.1.2`](https://github.com/vela-desktop/vela/releases/tag/v0.1.2).
[Vela 0.2](#02--personalization-productivity-and-architectural-hardening)
is next, with most of its features merged into `main` and release validation
ahead. The targets for 0.3–0.6 are provisional. Changes on `main` reach the
Beta channel through a tagged release. See [hardware coverage](hardware.md)
for the GPU and driver combinations tested so far.

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

**Support lifecycle.** The current Beta series receives maintenance fixes
until the next minor Beta is published. When `v0.2.0` ships, support for
0.1.x ends and fixes, including security updates, go to the 0.2.x series.
The updater in 0.1.0–0.1.2 cannot see `v0.2.0`, so those installations need
the [one-time updater step](release-channels.md#updating-older-beta-installations).
See the [release branch policy](release-channels.md#release-branches).
Maintenance releases take fixes only, without new user-facing features: new
work waits for the next minor release.

The [`v0.1.2` maintenance Beta](https://github.com/vela-desktop/vela/releases/tag/v0.1.2)
shipped on 16 November 2026. It made two explicit exceptions: Fedora 45
packaging ([#224](https://github.com/vela-desktop/vela/pull/224)), accepted
as a packaging extension, and a temporary choice between KWallet and GNOME
Keyring for app passwords pending Vela Secrets, backported as the fix for
[#222](https://github.com/vela-desktop/vela/issues/222)
([#255](https://github.com/vela-desktop/vela/pull/255),
[#277](https://github.com/vela-desktop/vela/pull/277)). Backported fixes
improved allocation recovery, renderer pass reuse, resource limits and
compositor security. They also stabilized text during
animation and scrolling ([#217](https://github.com/vela-desktop/vela/pull/217)),
prevented background apps from stealing focus
([#264](https://github.com/vela-desktop/vela/pull/264)), and reduced copying
in shared-memory screen capture
([#260](https://github.com/vela-desktop/vela/pull/260)). Other fixes kept
crowded taskbars navigable, preserved settings when writes failed, and
restored discovery of installed XDG icon themes
([#266](https://github.com/vela-desktop/vela/pull/266),
[#267](https://github.com/vela-desktop/vela/pull/267),
[#259](https://github.com/vela-desktop/vela/pull/259)). See the
[completed release tracker #225](https://github.com/vela-desktop/vela/issues/225)
for the backport inventory.

## 0.2 — Personalization, productivity and architectural hardening

Vela 0.2 need not wait for touch, pen or the on-screen keyboard.
Those projects are substantial and now provisionally target 0.3.
Distinguish **merged on main**, **candidate for 0.2**, and
**shipped in a tagged Beta**: these are not interchangeable statuses.

**0.2 features already merged into `main`**

- **Theme engine and personalization:** declarative themes, per-surface
  styles for the taskbar, Start, Quick Settings and notifications, and live
  reload for built-in and user-authored themes. The built-in themes include
  Window Glass and Vela Floating
  ([#207](https://github.com/vela-desktop/vela/pull/207)). Redesigned
  personalization with one-click theme galleries, previews, and Undo
  for applying or resetting themes landed in
  [#212](https://github.com/vela-desktop/vela/pull/212).
- **Personalization settings:** custom accent picker with exact hex input,
  shared 12/24-hour format across Vela, optional date and seconds on the taskbar,
  a notification count toggle, a Start icon chosen from the Vela sail,
  distribution logo or a custom image, and visibility options for pinned
  Start apps and desktop icons
  ([#216](https://github.com/vela-desktop/vela/pull/216)).
- **Vela Expressive:** optional shared controls and motion, a palette that
  can follow the wallpaper, a themed lock screen, independent Start and Quick
  Settings styles, and notifications grouped by app. Quick Settings tile
  states and the Do Not Disturb toggle are exposed to screen readers
  ([#244](https://github.com/vela-desktop/vela/pull/244),
  [#248](https://github.com/vela-desktop/vela/pull/248)).
- **Battery and screen sharing:** low and critical battery alerts for PCs
  and peripherals, with a configurable low threshold for the PC
  ([#215](https://github.com/vela-desktop/vela/pull/215)). The sharing
  indicator identifies the app and the screen or window being captured.
  Optional automatic Do Not Disturb holds back ordinary popups during
  sharing or recording
  ([#220](https://github.com/vela-desktop/vela/pull/220)). Notifications can
  also be dismissed with a right-click
  ([#188](https://github.com/vela-desktop/vela/pull/188)).
- **Taskbar:** window filters by desktop and display, keyboard-accessible
  previews on click, attention indicators, app badges and progress, a tray
  drawer configurable per icon, drag-to-pin, drag-to-raise, and placement on
  any screen edge. Super+1…9 and Super+0 open taskbar apps or switch virtual
  desktops, and can be turned off. In app mode, Shift launches a new
  instance and Alt opens the jump list
  ([#299](https://github.com/vela-desktop/vela/pull/299)).
- **Desktop and Files menus:** app-provided actions appear directly in the
  main menu, with per-menu visibility controls in Settings. Start uses the
  same menu for pinned apps and the full app list. The window menu can move
  a window to another virtual desktop or show it on all desktops
  ([#298](https://github.com/vela-desktop/vela/pull/298)).
- **Updates:** configurable automatic check intervals, the next scheduled
  check and a waiting-for-network status in Settings, and a manual recheck
  while an update is available
  ([#190](https://github.com/vela-desktop/vela/pull/190)). The updater shows
  pinned packaging recipes before installing
  ([#247](https://github.com/vela-desktop/vela/pull/247)) and discovers
  numbered Betas published as normal GitHub releases. Installations on
  0.1.0–0.1.2 need the
  [one-time updater step](release-channels.md#updating-older-beta-installations)
  ([#282](https://github.com/vela-desktop/vela/pull/282)).
- **Experimental virtual machines:** [QEMU/KVM with virtio-gpu and Vulkan
  Venus](virtual-machines.md) landed in
  [#251](https://github.com/vela-desktop/vela/pull/251) and supersedes the
  [nested Distrobox proposal #203](https://github.com/vela-desktop/vela/pull/203)
  as the way to try Vela on other hosts. Nested and full DRM/KMS sessions
  were verified on one Fedora 44 KDE guest. More host and driver combinations
  and the full GPU suites still need validation.

**Build, hardening and CI on `main`**

- **CPU requirements:** official x86 builds target x86-64-v3, and the
  session checks CPU support before starting the compositor. Local builds
  can select a different CPU target at configure time
  ([#245](https://github.com/vela-desktop/vela/pull/245)).
- **Reliability and CI:** stricter handling of notification images and
  markup, tighter rules for privileged system prompts, and parser fuzz
  tests in CI
  ([#288](https://github.com/vela-desktop/vela/pull/288),
  [#289](https://github.com/vela-desktop/vela/pull/289),
  [#290](https://github.com/vela-desktop/vela/pull/290),
  [#294](https://github.com/vela-desktop/vela/pull/294)).
- **NVIDIA GPU CI**, which has rented an NVIDIA RTX instance on Vast.ai for
  each run since 0.1.1 ([#199](https://github.com/vela-desktop/vela/pull/199)),
  now runs on a weekly rebuilt, digest-pinned image with a verified runner
  and an advisory gate for fixable package vulnerabilities
  ([#300](https://github.com/vela-desktop/vela/pull/300)).
- **Experimental Intel ANV CI** runs on a dedicated self-hosted Intel runner
  ([#226](https://github.com/vela-desktop/vela/pull/226)).

**Candidates to evaluate before 0.2, not automatically release blockers:**

- Finish or deliberately defer the existing
  [Snipping Tool #118](https://github.com/vela-desktop/vela/pull/118)
  (OCR, timer, color picker, recording) and
  [always-on-top #108](https://github.com/vela-desktop/vela/pull/108).
  Both are older draft PRs requiring compatibility work, review and tests;
  do not merge them solely to satisfy the roadmap.
- Evaluate configurable initial NumLock state requested in
  [community feedback #214](https://github.com/vela-desktop/vela/issues/214).
- Finish the desktop configuration fix
  [#235](https://github.com/vela-desktop/vela/pull/235) /
  [#236](https://github.com/vela-desktop/vela/issues/236).
- Optionally evaluate taskbar auto-hide and Start file/settings search from
  the [backlog](#next--not-yet-assigned-to-a-release) if the work is scoped
  and well tested.

**Release gate:** decide the final shortlist, freeze user-facing work and
validate the exact candidate commit with required AMD/NVIDIA GPU suites,
real Wayland login sessions, security/lock/crash recovery, keyrings,
themes, packaging and updater tests. Exercise the taskbar with physical
multi-monitor setups and apps that publish badges and progress. Investigate
[Ubuntu GPU failures #197](https://github.com/vela-desktop/vela/issues/197).
Follow the signed-tag rehearsal and
[release process](release-channels.md). There is **no fixed ship date**.

<a id="03--touch-pen-input-methods-and-on-screen-keyboard-indicative"></a>

## 0.3 — Input and experimental HDR (indicative)

The proposed input scope covers touch, pen, input methods and the on-screen
keyboard.

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

**Experimental HDR** is a parallel candidate for 0.3. The initial scope of
[renderer stage S7](renderer.md#11-stages) is 10-bit HDR output on compatible
DRM/KMS displays, with Wayland `color-management-v1` support for parametric
color descriptions and correct composition of SDR and HDR content. Define
reference white and brightness behavior, and verify SDR fallback, mixed
SDR/HDR displays and capture on physical hardware before shipping. The
input milestone can ship independently. Per-display ICC profiles remain a
later candidate under 0.5.

## 0.4 — Coordinated visual redesign (highly indicative)

Possible milestone for a distinctive, coherent default Vela visual language
across compositor-owned chrome, shell, Settings, Files and first-party
tools. The theme system and Vela Expressive already in main are
not themselves the coordinated default redesign. Candidate supporting UX
work includes Start's Recommended content and pinned-app folders, further
taskbar layouts such as uncombined buttons, system sounds, font and regional
settings, and printing/scanning integration. Which of these belongs in 0.4
is provisional.

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

- Per-display ICC profiles and broader color-management support.
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
| **Experimental HDR** (renderer S7) | [renderer §11](renderer.md#11-stages) | — | not implemented | 0.3 candidate |
| **Benchmark suite** | in PR | [#109](https://github.com/vela-desktop/vela/pull/109) | work/review ongoing | unassigned |

## Next — Not yet assigned to a release

These ideas are open for evaluation. The release sections above record any
provisional targets.

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

This is an overview of capabilities implemented on `main`. The release
sections above record when work was integrated or published.

- **Session**: login entry, systemd and D-Bus, portals, Xwayland, lock screen,
  idle, notifications, system tray, and Vela's own
  [polkit agent](polkit-agent.md) (User Account Control).
- **Updates**: the [Updates page](updates.md) in Settings, background checks
  and notifications, over `vela-update-daemon`, with configurable check
  intervals; Beta and Experimental
  [release channels](release-channels.md) on Arch, Fedora 44 and 45, and Ubuntu 26.10.
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
  snap layouts and Snap Assist, context menus with entries that can be
  hidden in Settings, light mode, a show-desktop strip, Caffeine, and file
  transfers in the taskbar.
- **[Taskbar](taskbar.md)**: placement on any screen edge, window filters by
  desktop and display, attention indicators, app badges and progress, a
  configurable tray drawer, drag and drop, paging when space runs out, and
  Super+1…9 and Super+0.
- **Personalization**: a [theme engine](theme-system.md) with declarative
  themes and per-surface styles, theme galleries with previews and Undo,
  accent, clock and Start icon settings, and Vela Expressive with a palette
  that can follow the wallpaper.
- **[Visual coherence](visual-coherence.md)**: the same pixels, clock and
  motion across compositor, shell and apps, and shared controls for all of
  Vela's interfaces.
- **Daily comfort**: clipboard history, snipping tool, touchpad gestures,
  mouse and touchpad settings, [battery alerts](battery-notifications.md) for
  the PC and peripherals, and a [screen-sharing indicator](screen-sharing.md)
  with optional automatic Do Not Disturb.
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
- **Virtual machines** (experimental): [QEMU/KVM guests](virtual-machines.md)
  with virtio-gpu and Vulkan Venus.
- **Languages**: English and Italian, switched live from Settings.

## Keeping this page current

This page changes in the same pull request as the work it describes:

- A new design document adds its row to **In progress**.
- The pull request that finishes a feature records it in the relevant release section.
- Update the supported Beta series in this roadmap when a new minor Beta ships.
- Distinguish merged main changes, proposed release candidates and
  actually published Beta features.
- Finalize the remaining 0.2 scope explicitly; review and either accept
  or defer old draft PRs, never merge them unconditionally.
- Mark all unreleased targets as estimates and revise them after testing.
- Work on a future redesign branch or independent font/icon repositories
  may begin early without merging unfinished designs into `main`.
- Keep **Done** as a summary of existing capabilities, and add each merged
  capability to it. List remaining work in **Next**; never re-list
  implemented features such as clipboard history as future work.

To propose a feature, open an issue that describes the behavior first
(see [CONTRIBUTING](../CONTRIBUTING.md)).
