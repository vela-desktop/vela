# Vela 0.1.2 backport inventory

This branch prepares the maintenance backport set agreed in release tracker
[#225](https://github.com/vela-desktop/vela/issues/225): bring reliability,
security, performance, testing, CI and packaging improvements from `main`,
while preserving the 0.1 feature scope. Fedora 45 and the credential-backend
compatibility fix are explicitly included.

## Baselines

- Maintenance base: `74350801549a58fe32559d93c9f293bd2d1566bf` (`release/0.1`).
- Source snapshot: `fdd45eab843597ab322d1dbd2caa9bb32d12f4e3` (`main`, 15 November 2026).
- 55 source PRs since the branches diverged: 25 maintenance backports,
  7 selective adaptations, 14 excluded changes and 9 already present.
- This is preparation, not a published release. `project(VERSION ...)` stays
  at 0.1.1 and the manual Arch Beta fallback stays at v0.1.1. Bump to 0.1.2
  separately after the final backports and review, before exact-revision release validation.

## PR decisions

“Backported” and “Adapted” describe the source inclusion decision; hardware
and distribution acceptance are recorded separately below. Source commit
provenance is retained in the backport commit messages.

| PR | Change | Decision | Included scope / reason |
| --- | --- | --- | --- |
| [#179](https://github.com/vela-desktop/vela/pull/179) | Release branches: CI on release/**, main is 0.2.0 | Adapted | Release-branch documentation only. CI and the package-version regression fix already exist; retain project version 0.1.1. |
| [#178](https://github.com/vela-desktop/vela/pull/178) | docs: record the v0.1.0 release and the release rehearsal | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#175](https://github.com/vela-desktop/vela/pull/175) | fix(audio): prevent volume wheel and knob from stalling on hardware readback | Already in release/0.1 | Do not apply again; already backported before this preparation. |
| [#184](https://github.com/vela-desktop/vela/pull/184) | fix(audio): add the device's half step to the readback tolerance | Already in release/0.1 | Do not apply again; already backported before this preparation. |
| [#182](https://github.com/vela-desktop/vela/pull/182) | fix(packaging): strip the Ubuntu DEB and compress it with xz | Already in release/0.1 | Do not apply again; already backported before this preparation. |
| [#183](https://github.com/vela-desktop/vela/pull/183) | test(updates): write the fake terminal's arguments atomically | Already in release/0.1 | Do not apply again; already backported before this preparation. |
| [#186](https://github.com/vela-desktop/vela/pull/186) | test(audio): read the default again when pw-metadata prints no keys | Already in release/0.1 | Do not apply again; already backported before this preparation. |
| [#189](https://github.com/vela-desktop/vela/pull/189) | fix(compositor): resize borders only where visible; the pointer follows changes at rest | Already in release/0.1 | Do not apply again; already backported before this preparation. |
| [#192](https://github.com/vela-desktop/vela/pull/192) | fix(compositor): leave a pointer an app locked where it is | Already in release/0.1 | Do not apply again; already backported before this preparation. |
| [#195](https://github.com/vela-desktop/vela/pull/195) | chore(deps): bump actions/download-artifact from 4.3.0 to 8.0.2 | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#194](https://github.com/vela-desktop/vela/pull/194) | chore(deps): bump actions/checkout from 4.2.2 to 7.0.1 | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#193](https://github.com/vela-desktop/vela/pull/193) | chore(deps): bump the github-actions-minor-patch group with 4 updates | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#199](https://github.com/vela-desktop/vela/pull/199) | ci: NVIDIA GPU suites on a rented Vast.ai container | Already in release/0.1 | Do not apply again; already backported before this preparation. |
| [#202](https://github.com/vela-desktop/vela/pull/202) | packaging: manual Beta builds fetch v0.1.1 | Already in release/0.1 | Do not apply again; already backported before this preparation. |
| [#188](https://github.com/vela-desktop/vela/pull/188) | feat(shell): dismiss hovered notifications with right-click | Excluded | New right-click notification dismissal. |
| [#207](https://github.com/vela-desktop/vela/pull/207) | feat(themes): theme files and Lowell137's surface styles, with Window Glass | Excluded | New theme engine and surface styles. |
| [#209](https://github.com/vela-desktop/vela/pull/209) | test(audio): fail, don't crash, when a stream leaves the snapshot | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#190](https://github.com/vela-desktop/vela/pull/190) | feat(updates): configurable automatic check interval and next-check time | Excluded | New update scheduling controls. |
| [#213](https://github.com/vela-desktop/vela/pull/213) | fix(scripts): headless and nested runs don't write the real session's cache | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#212](https://github.com/vela-desktop/vela/pull/212) | feat(settings): rethink Personalization: themes in one click, styles on their surface's page, real pictures | Excluded | New Personalization galleries and workflow. |
| [#216](https://github.com/vela-desktop/vela/pull/216) | feat(settings): custom accent, clock and notification count options, Start icon and pins, desktop icons | Excluded | New personalization and clock/Start controls. |
| [#220](https://github.com/vela-desktop/vela/pull/220) | feat: screen sharing indicator, with who and what, and Do not disturb meanwhile | Excluded | New sharing indicator and automatic Do Not Disturb. |
| [#217](https://github.com/vela-desktop/vela/pull/217) | fix(ui): stabilize text rendering through motion and transparency | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#215](https://github.com/vela-desktop/vela/pull/215) | feat(shell): battery notifications for peripherals and the PC | Excluded | New battery notifications. |
| [#226](https://github.com/vela-desktop/vela/pull/226) | ci: add experimental Intel ANV GPU runner workflow | Backported | Optional Intel hardware CI coverage, not a new mandatory release gate. |
| [#227](https://github.com/vela-desktop/vela/pull/227) | test(gpu): stabilize Intel/AMD functional suites and expose all capture failures | Adapted | Keep capture, input and RHI test fixes; omit sharing-indicator and new-personalization cases. |
| [#229](https://github.com/vela-desktop/vela/pull/229) | chore(deps): bump zizmorcore/zizmor-action | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#224](https://github.com/vela-desktop/vela/pull/224) | feat(packaging): add Fedora 45 support alongside Fedora 44 | Backported | Fedora 45 is the explicitly accepted packaging extension; include both RPM targets, updater routing, checksums and provenance. |
| [#221](https://github.com/vela-desktop/vela/pull/221) | fix(settings): keep shown values when vela.conf cannot be saved | Adapted | Keep save failure recovery and error UI for existing compositor settings; omit theme/style gallery integration. |
| [#230](https://github.com/vela-desktop/vela/pull/230) | docs: link Vela Matrix community from README | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#231](https://github.com/vela-desktop/vela/pull/231) | docs(readme): shorten the README and move the feature list to docs/features.md | Adapted | Keep the documentation structure; remove links to unshipped theme/sharing features. |
| [#148](https://github.com/vela-desktop/vela/pull/148) | Host allocation policy, steps 1–3 | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#243](https://github.com/vela-desktop/vela/pull/243) | Common allocation helpers, step 4 | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#245](https://github.com/vela-desktop/vela/pull/245) | x86-64-v3 CPU baseline | Excluded | Changes the CPU compatibility baseline to x86-64-v3. |
| [#244](https://github.com/vela-desktop/vela/pull/244) | Expressive controls foundation | Excluded | New Expressive controls. |
| [#247](https://github.com/vela-desktop/vela/pull/247) | feat(updater): show pinned package recipes during updates on all supported distros | Excluded | New package recipe preview. |
| [#248](https://github.com/vela-desktop/vela/pull/248) | feat(theme): Vela Expressive, a complete theme inspired by Material 3 Expressive | Excluded | New Expressive theme. |
| [#246](https://github.com/vela-desktop/vela/pull/246) | refactor: close the allocation failure contract (allocation policy step 5) | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#250](https://github.com/vela-desktop/vela/pull/250) | refactor(render): reuse passes so frames draw without allocating (allocation policy step 6) | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#253](https://github.com/vela-desktop/vela/pull/253) | fix(tests): keep private D-Bus buses and what they activate out of the host session | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#254](https://github.com/vela-desktop/vela/pull/254) | fix(updater): make recipe preview readable with a q-to-continue hint | Excluded | Only applies to excluded recipe preview #247. |
| [#251](https://github.com/vela-desktop/vela/pull/251) | feat(renderer): add experimental Venus VM support | Excluded | New Venus VM support. |
| [#257](https://github.com/vela-desktop/vela/pull/257) | docs: clarify CMake -D option syntax | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#255](https://github.com/vela-desktop/vela/pull/255) | draft(session): temporary explicit credential backend selection (#222) | Backported | Credential-backend selection fixes #222; omit CPU-v3 preflight, Venus environment handling and theme integration. |
| [#252](https://github.com/vela-desktop/vela/pull/252) | refactor: bound client-driven Vulkan memory and the icon cache (allocation policy step 7) | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#258](https://github.com/vela-desktop/vela/pull/258) | Compositor security hardening: IPC, privileged protocols, lock recovery, client buffers | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#259](https://github.com/vela-desktop/vela/pull/259) | fix: load XDG icons and stabilize GPU restart and shell tests | Backported | Keep XDG icon loading and runtime/test stabilization; omit theme-specific test cases. |
| [#260](https://github.com/vela-desktop/vela/pull/260) | perf(capture): fuse SHM alpha copies and document lower-cost GPU paths | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#261](https://github.com/vela-desktop/vela/pull/261) | fix: isolate FD-pressure tests and preserve Bluetooth power reversals | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#262](https://github.com/vela-desktop/vela/pull/262) | ci: optimize GPU builds, Vast.ai provisioning and NVIDIA parallelism | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#264](https://github.com/vela-desktop/vela/pull/264) | fix: prevent background chat apps from stealing focus | Backported | Retain the full maintenance change and its applicable regression tests. |
| [#265](https://github.com/vela-desktop/vela/pull/265) | fix: restore Start pins and final X11 positions, stabilize GPU CI | Adapted | Keep final X11 geometry, existing Start grid restoration and reusable harness fixes; omit Config.startPinned and new personalization tests. |
| [#266](https://github.com/vela-desktop/vela/pull/266) | fix(shell): handle taskbar overflow without covering app buttons | Adapted | Use the 0.1 taskbar, fixed 44x40 buttons and center/left alignment; port app/tray overflow navigation, clipping, minimize geometry and matching tests. |
| [#267](https://github.com/vela-desktop/vela/pull/267) | fix(settings): preserve saved preferences when writes fail | Adapted | Atomic persistence and independent errors for existing preferences. Port failure/recovery, concurrent-writer, malformed-file and real-control tests without new theme/clock/Start options. |
| [#268](https://github.com/vela-desktop/vela/pull/268) | test(shell): verify live time formats without screenshot equality | Excluded | Tests the excluded configurable clock formats from #216. |

## Dependency and adaptation notes

- Allocation policy order: #148 → #243 → #246 → #250 → #252, followed by
  #258 security checks and #261 resource-pressure/Bluetooth corrections.
- Allocation conversions apply to existing 0.1 sources. `capture_watch` and
  other sharing-indicator machinery are deliberately absent.
- Vulkan extent probing retains the image limits and upload budgets without
  the Venus buffer-export allocator or automatic Zink client configuration.
- The frame-path test calls the 0.1 blur API; theme saturation and extra blur
  controls remain excluded.
- Settings persistence covers the existing wallpaper/history, accent,
  appearance, taskbar, notifications, clipboard and compositor choices.
  Credential backend selection remains a bug-fix compatibility path.
- Taskbar tests exercise the real 0.1 QML with private service models, center
  and left alignment, app actions, navigation, clipping, tray growth, model
  shrinkage, active-app reveal and minimize targets. No theme engine is needed.

## CodeQL review follow-up

The nine path-expression findings on the first backport head are addressed by
upstream commit `a99584e`: configuration and icon metadata are read through
checked directory-relative opens. Parent traversal, control characters and
non-regular files are rejected; existing symlink aliases and close-on-exec
semantics are retained. The getline error-injection fixture also tracks
`fdopen`, preserving partial-read and OOM coverage. The original unchecked
reader fails the added traversal regression.

This corrective follow-up is additional to the 55-PR source inventory above.
GPU runs remain paused while CodeQL and then CodeRabbit review are completed.

## Review slices and follow-up

The oversized preparation diff was split into six draft PRs, each within
CodeRabbit's 100-file limit. Merge in order while preserving ancestry:

| Slice | PR | Scope | Changed files |
| --- | --- | --- | ---: |
| 1 | [#271](https://github.com/vela-desktop/vela/pull/271) | CI, isolation and UI foundations | 90 |
| 2 | [#272](https://github.com/vela-desktop/vela/pull/272) | OOM recovery and checked metadata | 64 |
| 3 | [#273](https://github.com/vela-desktop/vela/pull/273) | Allocation migration | 70 |
| 4 | [#274](https://github.com/vela-desktop/vela/pull/274) | Runtime, render reuse and credentials | 100 |
| 5 | [#275](https://github.com/vela-desktop/vela/pull/275) | Security, bounded resources and UI fixes | 98 |
| 6 | [#269](https://github.com/vela-desktop/vela/pull/269) | Inventory and release guidance | 4 |

Before review fixes, the reconstructed tree exactly matched `dae1642b`.
The subsequent differences address the CodeRabbit reviews:

- Correct release prerequisites, crash-recovery wording and fork-rehearsal setup.
- Publish readiness JSON atomically, clean up launcher process groups, thaw
  immutable test files before directory removal, and parse input actions with
  their operands, including after the shared-harness refactor.
- Use the checked opener for the second `index.theme` reader. A CPU regression
  rejects a FIFO without blocking and retains valid symlink aliases.
- Insert the supervisor's lock-state option before `--`; preserve operands.
- Detect empty/broken damage transforms before history is lost, and keep
  recovery pending until a replacement frame actually reaches the output.
  CPU negative controls reject all three former failure paths.
- Recognize parenthesized null `realpath` arguments, wait for the updater
  daemon's state acknowledgement, and include Debug-specific ASan flags when
  configuring preload order and allocation-interposer exclusions.
- Hold every original-compositor locker retry behind a registration barrier
  until that compositor is stopped; hold blackout exit with a one-shot barrier.
  Observe an already-held SIGSTOP even after a test-process pause, and require
  a confirmed lock-path failure injection. Their GPU execution remains part of
  the final hardware validation, after all reviews.

- Reserve child-supervision fallback timers before spawning; keep nonblocking
  exit observation and restart retries when pidfd/watch setup fails. Brightness
  cleanup retains exit supervision instead of blocking the compositor.
- Reject invalid latch timers and pace frame retries when latching is unavailable.
- Read the last duplicate credential preference and normalize duplicates on save.
  Verify credential providers after the startup hook returns, with login-token
  guards and session activation held until verification completes. A shared
  runtime lock serializes status/unit/target actions with login transitions;
  tests hold actions after token validation and verify logout waits for cleanup.
- Synchronize IPC queries with server-side descriptor cleanup, assert the updater's
  completion signal payload, and detect ASan from the effective C++ compiler flags.
  CPU regressions cover these paths; credential mocks wait for worker completion.
- Reject incomplete compositor startup if shell supervision cannot be reserved.
- Remove unpinned historical GPU claims from the allocation policy, and attribute
  upstream measurements without treating them as validation of this backport.

These review fixes are additional maintenance corrections, with no expansion
of the agreed feature scope. GPU workflows remain deferred until the complete
series has finished review; each current head must pass CodeQL first.

## Validation and release follow-up

Local validation (hardware runs remain deferred):

- Full native Debug build with GCC 16.2, Qt 6.12.0 and wlroots 0.20.2.
- 365 selected CPU/QML CTest entries pass on security-slice commit `8868189`
  after the runtime and credential-transition review fixes; the runtime slice independently passes 350.
  This includes documentation, reporter and preference fixtures adapted to 0.1.
- Debug-only `-fsanitize=address` configuration: the configuration-read test
  passes with the ASan runtime before the shim; the allocator-interposing
  damage test is excluded from that instrumented configuration. The global
  new-handler interposition test returns its expected skip under effective C++
  Debug ASan flags and passes in the native build.
- Updater/package contracts: 220 Python tests, 218 passed and 2 skipped because
  this Arch host selects the pacman backend in Debian-derivative detection cases.
- Before the review pause, the text-rendering RHI shader test passed all 15
  font/scale combinations with software OpenGL in offscreen mode. This is
  historical evidence, not validation of the subsequent review fixes.
- Settings stays running without QML errors in a five-second offscreen
  credential-setup smoke check with private HOME/XDG directories and D-Bus.
- Documentation, allocation-site policy, shared values, QML motion and diff
  whitespace checks pass.

These checks do not establish AMD, NVIDIA, Intel, physical KMS or live-session
correctness. Keep all existing test thresholds and no-skip hardware gates.

Before publishing:

- Complete required hosted CI and package/install checks for Arch/CachyOS,
  Fedora 44/45 and Ubuntu 26.10.
- Investigate or explicitly retest/reclassify Ubuntu GPU failures
  [#197](https://github.com/vela-desktop/vela/issues/197).
- Verify existing GNOME/KWallet credentials and session/PAM behavior on
  Fedora/GDM and Arch/SDDM, plus Settings, updater, lock/recovery and affected UI.
- After the final version-bump commit, run the complete mandatory AMD and
  NVIDIA GPU suites on that exact revision; Intel remains optional.
- Rehearse the signed tag pipeline on a public fork with that exact commit,
  including 0.1.1 → 0.1.2 updates, channel transitions and verification failures.
- Publish only after those gates, verify the assets, and then advance the
  manual Arch fallback. See [release channels](release-channels.md).
