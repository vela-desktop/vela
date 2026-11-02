# Automated desktop benchmark suite

One unattended campaign compares Vela with other desktop environments and
compares Vela releases with one another. It measures responsiveness, motion,
isolation from slow applications, fractional scaling and desktop overhead.
The same scenarios and metric definitions serve both comparisons.

> **Status:** implementation in progress, updated 2026-11-02. **P0 passed on
> the initial machine for the installed Vela and Plasma Wayland sessions.**
> All eight real-TTY qualification blocks passed, including independent
> calibrations and worker crash/hang recovery for both desktops. P1 discovery,
> planning, common block execution and checkpoint/resume are implemented.
> **P1 campaign-core acceptance passed on the initial Vela/Plasma machine:**
> a four-block multi-phase `--fast` exercise and a six-block normal exercise
> verified phase scheduling, complete repetitions and restoration.
> P2 neutral-client input, animation and paired GPU-producer workloads are
> implemented, with raw-data audits and independent-block statistics. Their
> six-block real-TTY `--fast` acceptance passed on the initial Vela/Plasma
> machine; the operator deferred normal three-repetition acceptance to proceed
> with P3. P3's native launcher/window/desktop observation workload passed
> real-TTY fast acceptance on that machine, across separate responsiveness
> and motion campaigns. Full family coverage, additional native shell/window
> scenarios and the application corpus remain incomplete. P4 now implements
> the native Wayland scaling pattern and UID-wide CPU/PSS measurements;
> physical acceptance is pending.
> No comparative performance results are included.
> Initial acceptance baseline: `eb85cb9`. The 2026-11-01 integration includes
> upstream `efdc9c6`; historical hardware acceptance does not qualify the new
> capture implementation or GPU-producer workload.

The [implementation guide](../benchmarks/README.md) documents current commands,
capability limits and restoration. Start without interrupting the desktop:

```sh
scripts/vela-bench check
scripts/vela-bench plan --fast
scripts/vela-bench run --phase check
scripts/vela-bench qualify --dry-run --target vela --target plasma
```

Python 3.11+ is required. `run --phase check` is read-only; standalone
`run --phase calibrate` is a supervised live campaign. `run --exercise`
rehearses selected measurement-phase slots with fixed instrument controls,
three repetitions by default or one with `--fast`. It does not measure those
families. Ordinary `run --phase responsiveness`, `motion` and `slow-apps`
requests execute P2's client scenarios; scaling/overhead execute P4's partial
pattern/resource workloads, with independently selectable or mixed phases.
`setup --tools-dir BUILD/tools`
installs the four fixed instruments and, when built, the bounded `vela-slowgpu`
producer with the dedicated account and
root-owned launcher. From a real local TTY, `qualify` requires one Start and
runs two independent instrument calibrations plus crash/hang recovery per
desktop, with automatic offline evidence/report export. Completed blocks
are hash-verified and retained by `--resume`; incomplete attempts restart
with fresh session/profile/calibration. Changes to installed suite/tool or
environment fingerprints require a new campaign. `session-test` remains a
focused lifecycle diagnostic; `--calibrate` adds controls to a single cycle.

### P0 acceptance evidence and scope

Real-TTY campaign `706ab1a3d71144959665b0b5c89b1397` passed on 2026-10-22.
Its offline export is
`benchmark-results/campaign-706ab1a3d71144959665b0b5c89b1397/`.
Independent inspection verified all 197 exported evidence-file hashes,
recomputed each block's acceptance audit from raw evidence and reproduced
its `p0-instruments-v1` qualification result. All eight blocks completed with
verified hardware restoration and no cleanup errors:

- Two fresh-profile calibrations per desktop verified full session activation,
  namespace isolation, primary/native/fixed/scale-1 display conditions,
  hardware brightness at 100%, performance power profile, native presentation
  feedback, F24 input, coarse output capture and the known 40 ms input-delay
  control. Both desktops passed the declared independent A/A tolerance.
- Worker crash and benchmark-UID hang cases passed for both desktops, including
  PID 1 recovery, input-device cleanup and verified service/hardware restoration.
  Their inner pilot result is deliberately `failed`; acceptance requires the
  injected failure and successful recovery, not a successful interrupted pilot.

This closes P0 for the initial AMD machine and these installed adapters only.
The controlled launcher currently requires Arch-style PAM and a supported
Plasma Login/SDDM Wayland greeter. GNOME, other PAM stacks and additional GPU
configurations require their own acceptance evidence before being supported.
CPU, headless Vela and private virtual KWin checks complement this campaign;
they do not substitute for physical PAM/logind/VT/device validation.

The neutral probe is a native `xdg-shell`/`wl_shm` Wayland client on both
desktops, with app ID `org.vela.benchmark.probe`; raw metadata records its
protocols. Vela's ext-image-copy-capture and Plasma's KWin ScreenShot2 routes
are qualified for **coarse state observation only**. Fine-grained desktop
motion remains P3 work. P0 does not establish optical input latency, desktop
rankings or completion of the five performance families.

### Implemented lifecycle and recovery contracts

After Start, the supervised worker activates the supported greeter's VT
before resolving/snapshotting outputs: inactive DRM compositors may announce
an empty inventory. Restore intent is journaled before the VT switch;
acquisition failure is recovered by PID 1. The first resolved hardware profile
is frozen in a write-once `conditions.json` bound to the immutable plan before
any target desktop launch or setting changes. Subsequent workers and resume
verify that resolution. Controlled runs apply/read back the agreed display,
hardware brightness and power profile, then restore them with verification.
No alternate display mode is silently chosen.

The display adapter resolves saved physical modes within the applying
KScreen process rather than reusing backend-local mode IDs. Enabled heads
with an unreported priority (`0`) remain enabled after the known ordered
heads; read-back verifies advertised ranks and every head's geometry, mode
and enabled state. Brightness recovery checks endpoint identity first and
omits a write if the original value is already verified.

Session units retain successful exits with `RemainAfterExit=yes` and failed
exits without `--collect` until owned teardown. Readiness detects early exits
and preserves actual exit properties. PID 1 opens a root-owned per-attempt
`*-launch.stderr` before dropping UID. Reserved Vela logs are collected after
stopping its processes and before profile deletion, without following links
and with a 128 KiB limit. Missing exit properties are unavailable evidence,
not evidence of success.

VT reservation masks both exact getty and autovt instance names, preserving
pre-existing masks. Alias `UnitFileState` is insufficient: it can describe
the canonical loaded unit while the alias template remains startable. Verify
the actual masks and inactive/masked states before switching VT; keep
`StandardInput=tty-fail` to refuse occupied terminals. A failed `systemd-run`
launch records bounded exit properties/session journal even before session
stdio exists. Campaign auditing retains that primary error instead of
requiring a completion receipt from a failed pilot.

Display-manager recovery retries `start-limit-hit` once only when the
immediately preceding state was verified `inactive` with `Result=success`.
It journals reset intent and clears only that service's transient counter,
without changing its rate-limit policy. Pre-existing failures, other errors
and repeated retry requests cannot reset the counter. The
`display-manager-recovery.json` artifact records the decision.

UID/runtime teardown verifies that the owned session unit is stopped,
captures its outcome, resets its failed state if necessary and waits for
unload before reusing its name. Active or foreign units are refused, and an
unload deadline prevents false cleanup success. The `*-session-stop.json`
artifact includes the original result captured before reset. Real user-manager
fixtures also reproduce the rate-limit and timeout/name-collision cases.

After `cleanup-failed`, code-only setup updates are allowed only when both
workers and all reserved UID processes/runtime are stopped. The pending
snapshot and evidence remain intact for `recover`. A recovery command retries
restoration; it does not change the interrupted pilot's failed result into a
successful run. See the implementation guide for recovery commands.

## 1. Agreed requirements

1. **Two comparisons.** Compare desktop environments on the same machine
   and detect improvements or regressions between Vela versions.
2. **One Start.** After initial setup, one invocation performs discovery,
   session changes, scenarios, repetitions, collection, cleanup and reporting.
   No manual login, window positioning, screenshot analysis or intervention
   between targets is part of a campaign.
3. **Warn before taking the machine.** Before ending the current session,
   show what will happen and an approximate duration. Session changes and
   any planned restart must be included in that notice.
4. **Portable by design.** Start validation on the current development
   machine, but discover hardware, displays, capabilities and installed
   sessions. No dependency on its GPU vendor, connector names or filesystem
   layout is allowed in the scenario definitions.
5. **Installed targets and archive comparisons in v1.** The suite measures
   the currently installed version of each supported desktop. It does not
   download, build, install or upgrade targets, or manage side-by-side Vela
   installations. Compare Vela versions by saving a run, updating Vela
   outside the suite and comparing the next run with that archive.
6. **Default experience.** Use each desktop's normal effects, animations,
   renderer and services. Control the experimental conditions without
   disabling effects to make desktop environments look alike.
7. **TTY, dedicated account and disposable state.** The operator starts the
   suite from a real TTY using their own login. The setup provisions a
   dedicated unprivileged `vela-benchmark` account with a separate UID.
   Recreate that account's home and user state before each independent
   phase/target/repetition block. The account persists between campaigns;
   profiles, processes and sessions do not. See §5.3 for its lifecycle.
8. **No graphical login between targets.** After the initial TTY login and
   start authorization, launch each full desktop session directly. No
   greeter, account selection, password prompt or manual session selection
   may interrupt a campaign. Do not configure display-manager autologin.
9. **Explicit, independently runnable phases.** The campaign has named
   execution phases with documented inputs, outputs and prerequisites.
   Run all phases with one Start, or select any measurement phase without
   manually preparing a session or running unrelated benchmark families.
   Progress and results identify the phase throughout execution.
10. **Three repetitions, or one with `--fast`.** Normal execution performs
    three complete independent repetitions for every selected measurement
    phase and target. `--fast` performs one, with the same scenarios,
    per-block sample budgets, warm-up, calibration and validity checks.

The five benchmark families are required scope. Gaming, Files performance,
long-term reliability, automatic builds and optical latency equipment are
outside v1. Recovery of the benchmark runner itself is part of v1. The
implementation choices in §4.2 keep that scope bounded without removing
any family or weakening measurement checks.

## 2. Questions the suite can answer

| Family | User-facing question | Primary evidence |
|---|---|---|
| B1 Responsiveness | How promptly does the desktop react? | Input-to-presentation distributions; separately, observed shell response |
| B2 Motion | How consistently does motion advance? | Presentation intervals, repeated visual states and interruption durations during active movement |
| B3 Slow applications | How much does another application's delay affect my work? | Change in B1/B2 with a controlled slow producer, then separate contention workloads |
| B4 Fractional scaling | Are window contents mapped correctly at my scale? | Deterministic pixel checks, geometry checks and original application captures |
| B5 Desktop overhead | What resources does the default experience use? | Session PSS, CPU time, available GPU measurements and separately scoped energy measurements |

Results remain separate. There is no combined desktop score, inferred
usability score or requirement that Vela wins. A fast internal function is
diagnostic evidence, not evidence of low input latency or smooth motion.

## 3. Existing evidence and first validation machine

Read [performance](performance.md) for the definitions and limits of the
historical observations. The following pieces can be reused or extended:

| Existing piece | Useful starting point | Missing for this suite |
|---|---|---|
| [`scripts/measure.sh`](../scripts/measure.sh) | PSS and CPU sampling; battery power where available | Whole-session accounting, target independence, repetitions and correct energy-source classification |
| [`tests/functional/harness.py`](../tests/functional/harness.py) | Isolated scenarios, lifecycle and assertions | Real desktop sessions and non-Vela adapters |
| [`tools/vela-input.c`](../tools/vela-input.c) | Automated actions in Vela's test environment | A common input path across desktop environments |
| [`tools/vela-pattern.c`](../tools/vela-pattern.c) and [sharpness tests](testing.md#sharpness-testing) | Deterministic pixels and window states | Portable capture, application cases and comparison across targets |
| [`tools/vela-slowgpu.c`](../tools/vela-slowgpu.c) and [slow-app tests](testing.md#slow-apps) | Explicit/implicit producer synchronization scenarios | Independent observation of a responsive second client on real outputs |
| `VELA_STATS=1` and the command socket | Frame-clock diagnostics and counters | Per-event samples and metric definitions independent of Vela |

The current headless harness is valuable for regression tests. Its virtual
vblank and Vela-specific counters cannot establish a physical-display
comparison with Plasma or GNOME.

Read-only inventory on 2026-10-18 found:

| Item | Observation |
|---|---|
| OS / kernel | CachyOS; `7.2.10-1-cachyos`, x86_64 |
| CPU | AMD Ryzen 9 9950X, 16 cores / 32 logical CPUs |
| GPU | AMD Navi 48; the project's hardware documentation identifies the development GPU as Radeon RX 9070 XT |
| Mesa / RADV packages | `26.2.4-1` |
| Installed normal Wayland entries | Vela and Plasma; GNOME was not found in the inspected session directory |
| Plasma / KWin packages | `6.8.0-1.1` |
| Active session | Vela |
| Output state | Two enabled outputs, logical sizes 2048×1152 at 125% and 1920×1080 at 100%; refresh was not established by this inventory |
| Power supply | A peripheral battery with `scope=Device`; no system battery was found in the inspected power-supply directory |

This is a discovery snapshot, not a benchmark result or a default campaign
configuration. The running Vela binary's identity is not established by the
repository HEAD. Collect both independently. Debug session entries such as
"No Scanout" or "Sync Test" are not normal default-experience targets.

## 4. Campaigns and target identity

The entry point is `scripts/vela-bench`, backed by a Python runner and small
native measurement helpers. The interface below is the intended complete
interface; use the implementation guide above for the currently available
subset:

```sh
scripts/vela-bench setup
scripts/vela-bench phases
scripts/vela-bench plan campaign.toml
scripts/vela-bench plan campaign.toml --fast
scripts/vela-bench run campaign.toml
scripts/vela-bench run campaign.toml --fast
scripts/vela-bench run campaign.toml --phase overhead --target vela
scripts/vela-bench run campaign.toml --phase overhead --target vela --fast
scripts/vela-bench run campaign.toml --phase responsiveness --phase motion
scripts/vela-bench run campaign.toml --phase calibrate
scripts/vela-bench status RUN_ID
scripts/vela-bench cancel RUN_ID
scripts/vela-bench resume RUN_ID
scripts/vela-bench report RUN_ID
scripts/vela-bench compare RESULT_A RESULT_B
scripts/vela-bench uninstall
```

`setup` prepares the restricted session launcher, permissions and dependency
checks, and provisions the dedicated benchmark account and its primary group.
It is idempotent and records the resources it owns. Account provisioning is
an explicit one-time setup operation, not an action hidden inside measurement.
It lists missing prerequisites; v1 does not run a package manager.
`plan` is read-only and can run without taking over
the desktop. `run` performs the complete campaign after the start notice.
Machine-readable equivalents must not require an interactive terminal.
`uninstall` stops owned benchmark resources and removes suite-owned setup
artifacts and the suite-created account/group. Saved result archives remain
unless explicitly selected for removal. It does not promise to erase system
logs or unrelated records of the installation.

A campaign selects installed targets, hardware-derived display profiles,
scenario versions, repetition policy, timeouts, required metrics and output
location. A built-in default campaign discovers installed supported normal
sessions; an explicitly requested missing target is a preflight failure.
Discovery never silently drops a requested target.

A target descriptor identifies its adapter, installed normal session entry,
executable identities, configuration baseline and version. V1 supports the
current installed Vela session and installed supported peer desktops, not
multiple concurrent Vela prefixes or arbitrary alternate-version launch
descriptors. The runner does not switch Git branches or replace
`/usr/bin/vela-*`.
Executables and resources must already be readable/executable by the
benchmark account outside protected personal home trees. Preflight reports
inaccessible artifacts; it does not change permissions on an operator's
home to make a target runnable.

For every target record package versions, reported application versions,
executable hashes and source revision/build flags when supplied by the
artifact. Never infer a binary's commit from the current checkout.

Historical comparison is a first-class v1 path: save a run, update Vela
outside the suite, run again and compare archives. Changes to kernel,
driver, libraries or configuration are reported as comparison differences;
they cannot silently become a claimed Vela-only regression. Alternating
side-by-side versions inside one campaign is outside v1. This restriction
does not change metric definitions or the archive comparison format.

### 4.1 Public execution phases

These are phases of a benchmark campaign. B1–B5 identify the measurement
families in §8; P0–P5 in §10 are development milestones, not commands for
running measurements. One-time runner setup is separate from both.

| Phase ID | Work | Inputs / prerequisites | Saved result |
|---|---|---|---|
| `check` | Discover hardware/installed targets, check prerequisites and resolve scope, budgets and configuration | Campaign and installed targets; no session takeover | Discovery, capability expectations, plan and estimate; runtime capabilities remain subject to launch verification |
| `calibrate` | Validate only the input, clock, capture, load or collector paths required by the selection | Passing checks and isolated real-session launch; no performance ranking | Fresh calibration evidence, measured characteristics, environment identity and uncertainty |
| `responsiveness` | B1: input response and native shell response | Input/presentation calibration and shell observation calibration | Raw response samples, failures, timing distributions and coverage |
| `motion` | B2: client animation and real desktop/window movement | Feedback/capture calibration suitable for the claimed resolution | Frame/motion observations, interruptions, uncertainty and coverage |
| `slow-apps` | B3: responsiveness/motion under delayed production and contention | Required B1/B2 measurement paths plus achieved-load verification | Fresh paired no-load/loaded observations and degradation |
| `scaling` | B4: pixel geometry across scales and window states | Verified display conditions and physical-pixel capture calibration | Original captures, pixel/geometry checks and application evidence |
| `overhead` | B5: idle and active resource cost | Process-inventory/counter validation; optional valid energy source | Resource samples, workload completion and optional scoped energy |
| `report` | Validate archives, aggregate selected measurements and generate the report | Archived samples and metadata; no desktop or privileged launcher required | Summary, coverage, comparison decisions and standalone HTML |

`phases` lists these stable IDs, descriptions, prerequisites and whether they
take over the machine. Implement the five measurement phases as a fixed
ordered list, with an explicit requirement set for each phase. Selecting
phases filters that list; flag order does not reorder it. Unknown phase IDs
fail before takeover. `run` without a phase selector executes checks,
required calibration, all five measurement phases and reporting. A single
`--phase` selects that phase; repeat the flag to select several. `--target`
narrows the resolved target set. `run --phase check` performs only host-side
checks; `run --phase calibrate` validates paths and then cleans up without
running performance scenarios. `report RUN_ID` is the standalone report
phase; `compare` consumes archives without repeating measurements.

Normal mode schedules three complete independent blocks per selected
measurement phase and target. `--fast` schedules one. Without phase selectors,
both modes run all five families and report their results. The flag only
changes repetition count: it does not shorten scenarios, lower sample rates,
omit controls, bypass required calibration, or reuse a previous live session.
It also combines with phase/target selection. Check/calibration/report work
is not itself repeated three times simply because normal measurement mode
was selected.

`plan --fast` uses the same selection semantics to estimate the actual one-pass
campaign. Persist the mode and resolved repetition count before authorization;
show them in progress and reports. Startup, checks, calibration and reporting
have their own costs, so fast-mode wall time is not promised to be exactly
one third. Resume preserves the original mode and plan; changing repetition
count creates a new run rather than rewriting an existing archive.

Every measurement-phase invocation automatically performs checks, selects
the phase's declared calibration checks, shows its duration/takeover notice and
manages the full session lifecycle. It finishes by archiving, reporting and
restoring the machine. Selecting B5 does not require the operator to launch
a desktop first, and does not run B1–B4. B5 does not calibrate screen capture
that it will not use. B3 collects its own paired controls during that phase;
an older B1/B2 result is not a replacement for its current no-load control.

Every invocation that performs live measurement calibrates the required
paths afresh. The block preparation sequence runs the relevant checks for
the current target/profile after launch and verification. Account for their
cost in the estimate. Calibration evidence is saved for the report, not
loaded from earlier campaigns to skip checks. Standalone `calibrate` records
diagnostics but does not create a reusable calibration cache for a later
`run`. Offline reporting/comparison needs no live calibration. Never assume
an earlier invocation has left a usable session or permission dialog open.

Each selected measurement phase has independent target/repetition blocks
with fresh isolated state and the same phase-specific warm-up and fixtures
whether invoked alone or through the complete suite. Do not make a phase
depend on caches, windows or services left by another family. Record the
actual block order and thermal/background conditions; fresh user state alone
does not make two runs identical. The full scheduler balances target order
within these blocks instead of relying on a manually prescribed sequence.

Each invocation resolves an immutable selection before measurement. A subset
creates its own run/child-run identity and scope; it does not overwrite an
earlier archive or silently fill gaps with samples from another environment.
`resume RUN_ID` revalidates the original plan and environment, preserves
completed blocks and reruns each incomplete block from a fresh profile and
fresh calibration. It does not restore a desktop, a window arrangement or
an event stream part-way through measurement. Close and validate a block's
sample files before writing its completion record atomically. Keep failed
or partial attempts as diagnostics, excluded from valid aggregates. An
incompatible environment requires a new run linked explicitly to the old one.

Expose phase boundaries in both the TTY and the event journal: phase ID and
name, target/version, condition, repetition, start, completion/status, elapsed
time and estimated remaining time. Include session setup/teardown as visible
substeps. Internal lifecycle operations are managed automatically; users
do not have to issue separate launch/stop commands to run one phase.

### 4.2 Agreed v1 implementation choices

| Area | Choice |
|---|---|
| Orchestration | One Python coordinator, a small privileged launcher with fixed local operations, and systemd supervision |
| Phase execution | A fixed ordered list of the five measurement families, explicit prerequisites and simple filtering for `--phase` |
| Calibration | Fresh required calibration for live execution; archived evidence is not a cross-campaign cache |
| Resume | Completed blocks are retained; an incomplete block restarts from a clean session, profile and calibration |
| Capture | One validated capture route per desktop adapter, with explicit version/capability checks and no automatic alternate route |
| Version comparison | Compare result archives across externally performed updates; one installed version per desktop per campaign |

Python implements the plan, sequence, sampling coordination and reporting.
Native helpers handle protocol/input/timing work where needed. Keep the
privileged launcher's operations small and fixed; reuse systemd's process
supervision, deadlines and stop hooks. The coordinator is the single owner
of the run state. No additional custom watchdog service or general-purpose
command server is required by this design. The initial host backend remains
systemd/logind, whose full graphical-session behavior must pass P0.

Persist metadata and summaries as JSON and event/sample streams as JSONL.
Generate static standalone HTML with inline assets/charts for reporting.
The suite needs no database, web server or frontend build pipeline. These
choices preserve local/offline operation, partial-result inspection and
independently runnable phases.

## 5. Start notice, duration and unattended lifecycle

### 5.1 Start notice

The operator logs into a TTY and starts the suite there. Changing VT does
not end a desktop that is still running on another VT: preflight discovers
it, and the notice includes its termination if required. Starting from a
TTY is the canonical v1 workflow; a graphical launcher is not required.

Before starting targets or terminating any session, persist and display:

- Targets, resolved scenario counts, display changes and repetitions.
- Which graphical session and conflicting user services, if any, will end;
  applications must be saved beforehand. The machine is reserved until
  completion even when no personal graphical session was running.
- Estimated duration as a range, its basis and a separate timeout budget.
- Result location, cancellation method and expected final state.
- Any planned reboot. Ordinary v1 campaigns need session changes, not
  reboots to change the kernel or driver.

The explicit Start action authorizes the displayed plan. A short cancellable
countdown precedes takeover. Obtain necessary administrative authorization
before that point; no authentication or permission dialogs may remain
pending during the campaign. Unattended callers must explicitly acknowledge
the same takeover notice in the invocation.

The estimate sums fixed scenario durations, warm-up, repetitions, startup,
configuration, capture and cleanup. Use previous timings only when their
hardware and campaign fingerprints match; otherwise use declared phase
budgets and label the estimate uncalibrated. There is no invented universal
"takes ten minutes" promise. Show revised remaining time in the persisted
status as actual phase timings arrive. Keep the permitted retry budget
finite and include it in the maximum expected duration.

### 5.2 Coordinator and session launcher

The Python coordinator must survive the graphical session it measures.
Run it under systemd outside that session, unprivileged, with a small
privileged launcher for fixed account/seat/session/restore operations.
Use fixed local launcher invocations and supervised unit operations; the
launcher is not a separate general-purpose command server. Workloads and
desktop launch commands run as `vela-benchmark`, never as root or as the
operator. The coordinator and launcher live outside the benchmark account's
user manager and teardown scope. That account has its own normal user
manager, user bus and `/run/user/UID`, independent of the operator's.

The initial launch backend is a PAM/logind session on a real VT, supervised
by systemd. It must create a genuine active local graphical login session,
start the target's normal session entry and verify seat access, user
services and D-Bus. Simply executing a compositor in a system service is
not sufficient. [systemd's execution documentation](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html)
describes the PAM and terminal mechanisms; the cross-desktop lifecycle
must be proven in stage P0 before implementation is built around it.

The launcher replaces the display manager's session-starting role for this
campaign. It starts the normal desktop session entry directly under the
benchmark UID in the disposable environment. Initial TTY login and
launcher authorization happen once; do not store the operator's password or
repeat authentication between targets. Session registration and real seat
access remain required, even though there is no graphical greeter.

The installed entries on the initial machine provide these launch paths:

| Target | Installed session launch command | Validation scope |
|---|---|---|
| Vela | `/usr/bin/vela-session` | Installed entry and launcher inspected; direct launch in the proposed isolated environment remains to be tested |
| Plasma Wayland | `/usr/lib/plasma-dbus-run-session-if-needed /usr/bin/startplasma-wayland` | Installed entry and D-Bus wrapper inspected; full isolated-session launch remains to be tested |
| GNOME Wayland | Resolve the installed normal GNOME session entry and its required environment | Not installed on the initial machine; a bare compositor process is not a full desktop-session substitute |

These paths are inventory examples, not constants for other machines. Use
desktop-entry argument semantics and adapter validation rather than evaluating
an `Exec` field as arbitrary shell text. Preserve wrappers and required
session metadata. GNOME also documents [its session launcher and systemd
integration](https://gnome.pages.gitlab.gnome.org/gnome-session/re01.html);
validate the installed version instead of assuming one command works on
every distribution.

Keep the display manager stopped between targets. A desktop that cannot
start through its validated direct-session adapter is unsupported; do not
fall back to a greeter, scripted password entry or persistent autologin
configuration. Handle first-run bootstrap in disposable state. Prevent
automatic locking during the run with a declared temporary idle inhibitor,
including it in the condition metadata; do not disable authentication or
change the operator's lock settings to avoid a prompt.

Stop the display manager only after the start notice. Release the original
graphical session and ensure it no longer competes for resources. Do not
terminate unrelated users' sessions: an occupied machine must fail its
takeover precondition. The launcher may operate only the registered campaign,
its sessions, temporary units, virtual input devices and the original
display-manager service. Target arguments are executed unprivileged and
are not evaluated as privileged shell text.

Use a fresh isolated profile for each target/repetition from a known
baseline. End the old target user manager and remaining target processes
before the next target. The coordinator lives outside that manager. Isolation
does not require replacing or reconfiguring the operator's user manager.
Record first-run bootstrap while preserving normal effects and services.

On completion, restore the initial display-manager service state and return
control to the originating TTY with the result location. A stopped display
manager stays stopped; one stopped for takeover is restarted. The suite
does not promise to recreate unsaved apps or unlock a user's session.
Snapshot the output enablement, modes and layout, the controlled monitor's
brightness and the active power profile before changing them. Restore and
verify those states on success, cancellation and recoverable failure. A
failed restoration is a visible campaign failure, even if samples were
collected successfully.

A native KScreen SetConfig rejection after monitor identity and physical-mode
validation permits one supervised restart of the original greeter. This handles
failures such as KWin choosing an amdgpu-rejected RA24 format without changing
the measured desktop's default environment. Persist the restart allowance
before stopping the service, refuse any open graphical user session or live
benchmark UID, then resolve the new greeter and verify the exact saved layout
again. Timeouts, missing modes, replaced monitors and read-back mismatches do
not permit this restart. Keep both attempts, bounded operation diagnostics
and observed/requested output inventories in `conditions-recovery.json`.
A second rejection remains a restoration failure; do not select another mode
or promote the block's samples to accepted results.

### 5.3 Dedicated account and disposable home

Use one dedicated account, `vela-benchmark` by default, with a stable UID,
a primary group and a fixed suite-managed home path. The provisioning uses
normal local-user session semantics so that PAM/logind and desktop policy
can create a complete graphical session. Do not assume a system-service UID
or `DynamicUser` behaves like a normal desktop login account.

The account has no administrator privileges or sudo/wheel membership. Its
password is locked and there is no operator-facing password or autologin
configuration. The authorized launcher opens its local session without asking
for that account's password; this path must be validated against the host's
PAM policy. Grant device access through the active local session where
possible and record any additional permissions required by the backend.
The suite does not install remote-login credentials for the account.

`setup` records the created account/group IDs, home path and ownership of
managed directories. If the account name or home path already belongs to
an unrelated installation, fail with a clear conflict instead of adopting
or clearing it. A compatible recorded setup can be reused idempotently.
The account is reserved exclusively for suite sessions; concurrent use and
concurrent campaigns are rejected. Keep lingering disabled so that services
do not intentionally survive the end of its sessions.

A fresh profile is part of every independent phase/target/repetition block,
not just the first target in a campaign. Each block receives a new private
work area holding its home, configuration, application data, state, caches,
temporary files and runtime sockets. Start from an explicit environment allowlist, with a
known working directory, instead of inheriting the TTY shell's environment
or sourcing the operator's shell startup files.

Before opening the first PAM session for a block, recreate the managed home
at the account's declared home path from the known empty/bootstrap baseline.
`HOME` and NSS home lookup must resolve to that same fresh profile. This
also gives a newly started user manager the correct home before it loads
user units or activates services. Keep XDG configuration/data/state/cache
paths within this home. A temporary `HOME` assigned only to the compositor
would still leave indirectly started services inconsistent.

Use the benchmark account's native runtime directory and user bus. Runtime
data need the ownership, permissions and lifetime required by the
[XDG Base Directory Specification](https://specifications.freedesktop.org/basedir/latest/).
Never inherit personal D-Bus, Wayland, X11, SSH-agent, keyring or PipeWire
endpoints from the operator. Preserve distribution-level XDG search paths
so that installed desktop defaults and applications remain available.

The [systemd user manager and runtime directory are per user](https://www.freedesktop.org/software/systemd/man/latest/pam_systemd.html).
A dedicated UID supplies the normal separation from the operator. It still
requires teardown between sequential targets that reuse the benchmark UID:
stop all its sessions and services, wait for its user manager to exit, verify
that no owned process remains, and verify runtime cleanup before rebuilding
the next home. The launcher may terminate this reserved account as a unit;
it must never apply that operation to the operator. Do not reset an account
home while processes can still hold files or activation state from it.

A different UID does not make world-readable personal files inaccessible.
Workloads must not enumerate or use personal home trees. Where filesystem
permissions alone do not enforce the required exclusion, hide personal
homes from the benchmark desktop and its user-manager/service tree using
the same declared filesystem policy on all targets. Preserve the benchmark
home, native runtime and real device access. P0 verifies the exclusion with
fixtures; it does not require an alternative same-UID systemd architecture.
Normal target services remain enabled.

All desktop and client writes belong in the disposable work area. Only
explicit result artifacts are exported after collection. Before deleting
the area, stop every process/service that can still reference it. Benchmark
account capture permissions that require persistent state are runner-managed
setup artifacts, separate from the target profile and the operator's state.
The account/group and documented setup artifacts persist until explicit
uninstallation. Do not create/delete an account for each repetition, which
adds provisioning changes unrelated to the workload being measured.

Disposable means removing owned profiles, temporary units, mounts, sockets
and processes, plus restoring service state. It is not a claim of no trace
anywhere on the computer: benchmark results and normal system logs are
expected to remain. Record persistent runner/setup artifacts explicitly.

Use the same declared storage policy across targets. A disposable home need
not be a RAM filesystem: moving caches/data to RAM changes the experiment.
An empty profile also does not reset the kernel page cache, GPU/driver state
or machine temperature. Declare the warm-up policy and keep cold-start and
steady-state conditions separate. The isolation claim concerns personal
files, settings and services, not a fresh boot or a different kernel.

P0 tests use synthetic personal-state fixtures: distinctive settings,
autostart entries, home lookups and service activation. Demonstrate that none
enter the measured session, that writes remain disposable, and that changes
in target A do not reach target B or the next repetition. Check both paths
based on `HOME` and paths resolved through NSS, plus indirect service
activation. Do not inspect real personal documents to establish this.

### 5.4 State machine and failures

Persist each transition atomically, including the public phase and block
identity defined in §4.1:

```text
planned -> authorized -> takeover -> launch -> configure -> verify
        -> stabilize -> warm-up -> measure -> validate -> archive
        -> teardown -> next phase/target/repetition -> report -> restore -> done
```

Every phase has a timeout. Use systemd supervision, unit deadlines and
idempotent stop/cleanup hooks to terminate a stuck target even when its
event loop or session agent cannot respond. The coordinator handles
phase-level deadlines, while systemd bounds the supervised execution and
cleanup if the coordinator itself stops responding. P0 must demonstrate
both failure paths; do not add a bespoke watchdog daemon. Identify
owned processes by session/cgroup and PID identity, not broad process-name
kills. Checkpoint raw samples before advancing. A restarted compositor or
unexpected session change invalidates the affected repetition; do not
stitch the two intervals together as an uninterrupted result.

On cancellation or failure, release virtual input, stop owned workloads,
archive partial evidence, tear down the target, restore the previous
display-manager state and return to the originating TTY where possible.
A persistent recovery record enables cleanup after coordinator
restart or the next boot following an unexpected power loss. Resuming an
interrupted campaign revalidates the environment and repeats incomplete
blocks; it never treats partial measurements as completed repetitions.
Kernel/driver failure that prevents recovery cannot be hidden as success.

No planned reboot recovery is required for v1. A future campaign requesting
it must include the reboot count and automatic resume in its initial plan.

## 6. Portable adapters and default experience

The scenario engine uses a small adapter contract:

| Operation | Contract |
|---|---|
| Discover / probe | Identify installed targets and capabilities without inventing support from a desktop name |
| Launch / stop | Run and terminate the complete real session, verify health and owned resources |
| Configure / verify | Apply display conditions and read back the actual output state |
| Act / observe | Reach the requested UI state and observe its success, without counting command acknowledgement as presentation |
| Capture | Supply documented pixel format, coordinates, timing and observation limits |
| Inventory | Identify the complete desktop process set and its effective configuration |

Initial adapters are Vela, Plasma Wayland and GNOME Wayland. Additional
adapters must not change the meaning of a shared metric. Desktop-specific
diagnostics may be attached but cannot replace missing common metrics.

Portability means capability discovery and no machine-specific assumptions.
It does not make Vela run on a GPU without its required Vulkan support,
make every desktop expose every capture protocol, or make an unknown
session-launch backend validated. Unsupported conditions produce explicit
reasons and a useful preflight report. Never replace a physical DRM run
with nested/headless execution without changing its experiment identity.

The primary profile uses fresh isolated user state with the desktop's
distribution defaults. Record those defaults, the distribution and relevant package
configuration; do not describe them as universal upstream defaults. Do not
copy the operator's settings, extensions, autostart entries or application
history into that state.
Keep animations, shadows, blur, renderer selection and normal services.
Do not enable Vela debug paths, disable scanout or force software rendering
for the primary comparison. Optional diagnostic variants have separate IDs.

Control output modes, scaling, brightness, power profile, application
versions, dataset, window count and scenario actions. Hardware discovery
proposes an output profile before takeover; each target must apply and
verify it. If it cannot, mark that condition unsupported rather than
silently switching to another refresh or scale. Record VRR's actual state
and keep fixed-refresh and VRR experiments separate.

The agreed v1 hardware conditions are:

| Condition | Required state during measurement |
|---|---|
| Output | Only the machine's resolved primary physical monitor enabled; all other outputs disabled |
| Brightness | Primary monitor at 100% of its hardware brightness range |
| Power profile | Performance, through a supported and verified platform power-profile mechanism |
| Display mode | The same resolved physical resolution and fixed-refresh mode across compared targets |

Resolve the primary monitor once in the campaign plan, rather than letting
each fresh desktop choose a different one. Connector names are discovered,
not hardcoded. Read back the applied conditions before the measured interval;
configuration and brightness-control traffic do not belong inside that
interval. A missing brightness/profile control or failed read-back makes the
requested condition unsupported; do not report an unverified condition as
100% brightness or silently substitute a CPU governor for a platform profile.

Fractional scales vary only within the scaling phase. Mixed-monitor
benchmarks are outside v1. These controlled hardware conditions do not
uniform the desktops' effects or animation settings.
Logical and physical window dimensions are both recorded; workloads at
different resolutions are different conditions. Connected-device changes,
power-source changes or unrequested output changes invalidate a block.

## 7. Measurement foundations

### 7.1 Input, clocks and presentation

Use one common Linux virtual-input path for primary cross-desktop input
tests. The proposed backend is [uinput](https://docs.kernel.org/input/uinput.html),
which creates kernel input devices. Keep the device and event scheduler
alive across actions; spawning a CLI per event must not become part of
the measured latency. Record the injection syscall interval, delivered
event timestamp and action ID. Probe device handling and permissions.
Vela's virtual-keyboard/pointer tools remain useful for isolated tests.

A neutral client acknowledges input with an identified visual update and
requests presentation feedback for that update. Record input receipt,
commit, feedback timestamp, discarded updates, feedback flags and output.
Read the presentation clock ID. Subtract timestamps only after establishing
a common clock domain and documenting conversion uncertainty. Preserve
the raw timestamps and clock identities.

Label this metric **software input injection to reported presentation**.
It excludes a physical switch, USB polling, panel scanout position and pixel
response. Vela's internal "draw to light" statistic is not a substitute.
Likewise, [`wl_surface.frame`](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_surface-request-frame)
requests a drawing hint; it is not an optical presentation measurement.

### 7.2 Observing desktop motion and shell UI

Presentation feedback for the neutral client cannot by itself observe a
launcher appearing, another surface animating or a static buffer moving.
Those scenarios require output observation or a separately identified
desktop-specific trace. Do not infer whole-desktop smoothness from a probe
client's frame cadence.

Each adapter declares one validated capture backend for its supported
desktop versions. That backend provides its still-image/stream operations
through one declared capture route. Probe the version, physical-pixel
geometry, sample rate, timestamps and permissions. A missing or failing
route makes its dependent measurements unsupported/failed as appropriate;
do not automatically retry through another API, protocol or recorder and
silently change the experiment. No DE-agnostic capture fallback is required
in v1. Record the chosen route and its limitations in every relevant result.

P0 selects and validates the route for each adapter. If a proposed route
uses portal capture, account for its permission dialogs and persistence
limitations; a restore token does not guarantee unattended access. See the
[ScreenCast interface](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html).
Permission bootstrap belongs in setup, with tokens managed separately from
immutable profile snapshots. A route that still asks the operator during
measurement does not satisfy the unattended contract and cannot be marked
supported. A different backend in a future release requires explicit
validation and a distinct observation-method identity.

Capture is itself a workload and may affect composition/scanout. B1's
client-feedback timing and B5's primary resource runs have capture off.
Captured shell/motion observations use their own condition IDs. Measure
collector overhead and capture cadence in a pilot. Do not rank a 180 Hz
desktop's missed physical refreshes from a 60 fps stream. Where cadence or
timestamp uncertainty is insufficient, provide only the coarser observation
that is justified, or mark the requested metric unsupported.

Use visual frame IDs and known motions in a neutral calibration client to
detect capture duplicates, drops, scaling and sampling aliasing. Adapters
must pass the same calibration tolerances before their output can appear
in a shared chart. Exact tolerances are fixed from P0's pilot and versioned
before performance comparisons; a different observation method is not
silently merged into an existing series.

### 7.3 Stabilization and repetition

The agreed repetition policy is three independent complete blocks in normal
mode, one in `--fast`, under §4.1's lifecycle. Interactions and samples within
a block are not additional independent repetitions. Keep all per-block
durations and scenario parameters identical between modes.

Warm-up and stabilization have bounded durations and explicit checks:
session readiness, no first-run dialogs, settled application state, output
configuration and sampled background activity. A busy desktop is reported
as such; do not wait indefinitely or switch off its normal services.
Transient startup activity is recorded separately from steady state.

Use a recorded random seed and alternate/balance target order across
independent session blocks. Input phases must cover the refresh period
rather than always landing at a favorable offset. Keep client binaries,
datasets and scenario definitions fixed across targets. Calibration may
adapt to hardware but cannot retune the workload separately to favor a
desktop. Save calibration parameters and measured load characteristics.

Report per-block results, median and spread. Retain every sample and
timeout; provide sample counts and uncertainty for comparisons. Adjacent
frames are correlated, so uncertainty must respect independent blocks.
Only report tail percentiles when the pilot-established sample budget
supports them; five interactions do not establish p99. Thresholds for a
meaningful regression are fixed before the compared runs, in both absolute
units and relative change where appropriate. Observed differences within
measurement uncertainty are inconclusive.

A fast run still reports valid per-event measurements and within-block
distributions where supported, but cannot estimate variation between
independent repetitions. Label its evidence as a single repetition; do not
invent between-run spread, pool old runs to simulate missing repetitions or
treat it as repeated confirmation of an improvement/regression. Three normal
blocks also do not automatically guarantee enough evidence for every tail
estimate or small performance difference.

### 7.4 Validate the measuring instruments

Calibration and suite qualification must establish that the measurement
paths detect known changes and reject invalid observations:

- **A/A control:** compare the same installed desktop/helper versions under
  the same conditions in independent blocks. Estimate repeatability and
  verify that the comparison logic does not systematically invent gains
  or regressions from noise. Identical samples are not expected.
- **Sensitivity control:** introduce a known delay or interrupted movement
  in the neutral test fixture and verify that the relevant metric detects
  it within its declared resolution. The controlled defect belongs in the
  fixture, not in a primary desktop's configuration.
- **Invalid-data controls:** lost actions, missing frame IDs, wrong capture
  dimensions, incompatible clocks and truncated samples must be identified
  as failures/invalid data, not exceptional performance.

Archive these controls with the qualified metric/helper version, backend,
conditions, tolerances and sample budget. Execute the relevant live
calibration checks afresh under §4.1. Qualification evidence is retained
for audit; it is not a cache that bypasses those checks. The full A/A
qualification is a separate qualification exercise and does not need to
double every ordinary campaign. Fix acceptance tolerances
before interpreting performance comparisons; do not tune them to make a
preferred desktop win. Reports identify which qualification evidence was
used and any missing validation.

## 8. The five benchmark families

### B1. Responsiveness

**Hypothesis:** Vela responds promptly and consistently at the tested refresh.

- Primary scenario: a focused neutral client changes an identified patch
  on each injected key/click, using constant application work.
- Shell scenarios: launcher, Alt+Tab and a context menu, with each adapter
  mapping equivalent actions to verified UI states. Keep their distributions
  separate; different shell tasks do not become one interchangeable test.
- Report software injection-to-presentation and, separately, client
  receipt-to-presentation. For captured shell UI, report the first observable
  response interval at the capture method's resolution, not optical latency.
- Measure first visible response separately from usable/final state and
  animation completion. Detection thresholds must tolerate normal alpha
  fades without treating an invisible pixel difference as a usable panel.
- Failures include missed actions, focus loss and absent/ambiguous responses.
  Report their count; never compute a flattering latency only over successes.

The client-feedback path and at least one native shell response, initially
the launcher, are primary v1 scenarios. Shell observations require a validated
capture path. Other shell actions contribute additional explicit coverage.

### B2. Motion

**Hypothesis:** visible movement advances regularly throughout active motion.

Run a neutral animated client, then controlled window dragging/resizing,
snap/maximize transitions, shell panel motion and desktop switching. Keep
client motion and compositor/shell motion separate. Verify final geometry
and state so a failed action cannot look like a perfectly smooth empty run.

For the neutral client, collect presentation intervals, committed/presented/
discarded frame IDs and interruptions. For observed desktop motion, track
window/panel position or another verified visual state over time, including
repeated states and longest observed pauses. Do not compare distance per
frame without accounting for different default durations and easing.

Count opportunities for updates only during a declared active interval.
A stationary desktop that intentionally stops rendering has no missed
animation frames. Fixed-refresh experiments can express gaps in refresh
periods; VRR experiments need separate definitions and measured intervals.
The capture path's resolution limits all claims about desktop motion.

Both neutral client motion and at least one real compositor/shell movement,
initially window dragging, are primary v1 scenarios. A client-feedback-only
implementation does not complete this family. If an observation can establish
only long interruptions, label and compare that coarse metric; it cannot
establish frame-by-frame regularity at a finer refresh period.

### B3. Isolation from slow applications

**Hypothesis:** a slow producer has a bounded effect on the responsive client
and on observed desktop interaction.

Pair every workload with a no-load B1/B2 control in the same configuration:

1. A producer with delayed buffer readiness, explicit synchronization.
2. The corresponding implicit-synchronization case when supported.
3. CPU contention and GPU contention as separately identified workloads.

Extend the existing slow-GPU fixture with machine-readable achieved delay,
submitted/completed work and device identity. Begin with the existing
150 ms case, but verify the achieved condition. A separate compute queue
alone does not prove an absence of GPU contention; measure and report the
remaining interference. Keep a responsive foreground client visible.

Report absolute latency and motion results, degradation relative to the
paired control, failure counts and achieved load. Vela's held-commit/missed-
vblank counters are supplementary diagnostics. Moving only the hardware
cursor cannot establish that the composited desktop stayed responsive.

The fixture must verify synchronization and continued buffer production.
An unsupported explicit-sync path is not a successful test with zero stalls.
Use intentionally broken behavior as a sensitivity control only in isolated
development tests, never as a target's primary default configuration.

### B4. Fractional scaling

**Hypothesis:** the desktop preserves intended physical-pixel geometry across
supported scales and window-management states.

Use native-output captures of the deterministic pattern and a controlled
application corpus. Begin with 100%, 125%, 150%, 175% and 200% where the
output and target support them. Exercise normal, snapped, maximized and
restored windows on the primary monitor. Mixed-monitor transitions are
outside the agreed v1 hardware profile.

For the pattern, record requested/preferred scale, actual buffer dimensions,
viewport, expected destination geometry and observed pixels. Report geometry
errors separately from pixel-value differences. Exact pixel equality applies
only to declared regions and a compatible color pipeline; document any
color transform, tolerance and excluded decorations. Do not align away an
unexpected resize or resample a capture to make it pass.

For real applications, use fixed Qt, GTK, browser and Xwayland versions,
fonts, content and client rendering settings. Archive original captures,
physical-pixel crops and automatic geometry checks. Differences in font
rasterization are not automatically compositor defects or a universal text
sharpness score. A corpus case lacking a sound pixel oracle is visual
evidence, not a pass/fail quality ranking.

Verify scaling and completed state before capture. Missing fractional-scale
support or a scaled capture stream is
reported explicitly. All positioning, captures and analysis are automatic;
human inspection is optional review of the resulting artifacts.

### B5. Desktop overhead

**Hypothesis:** the default desktop has low overhead at rest and during use.

Run three separate conditions with capture and verbose tracing off: steady
idle, a fixed interaction sequence and a fixed local application workload.
Use identical datasets and application versions without network dependency.
Verify scenario progress and completion so doing less work cannot masquerade
as better efficiency. Keep baseline and load conditions separate.

Define the desktop process inventory before sampling: compositor, shell,
helpers, portals, isolated session user manager and other target services.
Include processes launched outside the compositor's child tree through
D-Bus/systemd. The reserved UID provides an inventory boundary; classify
its services, workloads and collectors by session/cgroup and verify that
no prior target remains. Track PID start times, exits and restarts. Record client
workloads and benchmark collectors separately and list every exclusion.
Shared system services appear in a separate inventory; their complete cost
cannot be attributed to one desktop by summing its user processes.

Report PSS in MiB and CPU user+system time over elapsed time, with CPU percent
explicitly normalized to one logical CPU. PSS is process memory accounting,
not total system memory or total VRAM. GPU utilization, memory and energy
require documented backend scopes, units, sampling and permissions. Missing
GPU counters do not make a machine unusable for the other measurements.
Voluntary context switches are named as such, never total wakeups.

Energy has independent source classes:

- A discharging **system** battery can measure whole-computer energy/power.
  Exclude `scope=Device` peripherals; validate ambiguous sources. Integrate
  over a sufficiently long interval and record state-of-charge, brightness
  and power source. Do not extrapolate a brief sample into guaranteed runtime.
- CPU/package or GPU energy counters measure their stated component only.
  They cannot be relabeled whole-computer watts or summed across overlapping
  domains. Detect counter wrapping and unavailable permissions.
- An already configured external meter can provide a separate whole-system
  series. No meter is required for v1 to run.

No valid source means `unsupported` for that energy metric, not zero watts.
In particular, the initial machine's peripheral battery cannot establish
computer power consumption. Report collector overhead and sampling rates;
do not silently subtract estimated costs from published measurements.

## 9. Results, coverage and comparison rules

Each run saves a self-contained local archive:

```text
manifest.json       schema, suite revision, target and environment identities
plan.json           selected phases/targets, fixed requirements, order, seed, estimate
events.jsonl        phase boundaries, lifecycle, failures and actual timings
calibration/        fresh calibration evidence for each target/profile/block
samples/            raw timestamped JSONL metrics by target/scenario/repetition
blocks/             completion records and separate attempt identities
captures/           original images and relevant capture metadata
logs/               owned session, workload and collector logs
summary.json        aggregates, coverage, validity and comparison decisions
report.html         static standalone HTML with inline assets/charts
```

Include hardware/driver/kernel/library versions, output modes and scales,
session backend, actual settings, client identities, collector identities,
units, clock domains, sample counts, warm-up policy and limitations. Record
configuration hashes, the benchmark-account UID/GID, baseline identity,
storage/warm-up policy and relevant resolved values, not a dump of personal home
directories. Keep data local; publishing is outside the suite.
JSON/JSONL are the canonical machine-readable formats. Report generation
reads those local files and requires no database or web service. A block
completion record identifies its validated files and attempt, so recovery
can retain completed work without reconstructing application state.

Every planned metric has an explicit status:

| Status | Meaning |
|---|---|
| `valid` | Measurement completed and passed its validity checks; says nothing about which desktop wins |
| `invalid` | Data exist but a required condition or observation check failed |
| `failed` | Launch, action, workload or collector failed or timed out |
| `unsupported` | A required capability or valid measurement source is absent |
| `not-run` | Planned work did not execute, including cancellation |

Declare mandatory and optional cases in the immutable plan. Missing mandatory
results prevent a complete campaign and produce a nonzero exit status.
Optional unsupported metrics leave visible coverage gaps. Never label an
entire family validated because one easier subcase passed. Completion of
the execution and completeness of the scientific comparison are separate
fields. Record normal/fast mode, planned repetitions and valid repetitions
per condition. A valid fast run may complete all five families while still
having only one repetition; coverage completeness and replicated evidence
are separate report properties. A successful explicit subset is complete
for its declared selection. Unselected families are identified as not
selected and are not failed work.
The report must not label that subset a complete five-family run. Report
`execution_complete`, `selection_complete` and `full_suite_complete`
separately. All five families need valid primary scenarios before calling v1's
core campaign complete. The primary matrix includes native shell response
and real window movement; neutral-client results cannot substitute for them.
Show coverage per target, scenario and display condition. A target missing
a mandatory observation can still produce a partial report but cannot be
declared fully compared or supported for that campaign.

Compare only compatible metric/scenario versions, observation methods,
profiles and environments. An environment mismatch appears prominently and
blocks an unqualified improvement/regression claim. Reports may show such
runs side by side with their differences. Cross-machine results are separate
strata, not automatically pooled rankings. Use absolute units as well as
relative changes; relative changes from near-zero baselines need care.

## 10. Implementation stages and acceptance

| Stage | Deliverable | Acceptance evidence |
|---|---|---|
| P0 Feasibility | Prototype TTY lifecycle under the dedicated benchmark UID, profile/runtime reset, direct session launch, common input/feedback, capture and overhead calibration | On the initial machine: TTY → Vela → Plasma → TTY using the setup-managed account, with no graphical greeter; previous display-manager/output/brightness/power-profile states restored; operator/previous-target fixture state does not leak; crash/hang recovery; verified capture cadence/geometry and instrument controls (§7.4). Validate the same contracts on GNOME before calling its adapter supported |
| P1 Campaign core | Fixed phase list/requirements and CLI, checks, start notice/estimate, Python coordinator, fixed privileged launcher, systemd supervision, JSON/JSONL state and static reports | Missing targets fail before takeover; warning precedes termination; phases run independently; default schedules three blocks and `--fast` one with identical per-block work; fresh calibration runs; interrupted blocks restart cleanly while completed blocks remain intact |
| P2 Primary measurements | B1 neutral client, B2 client motion and B3 paired slow-producer tests | Repeated real-output runs with raw data; common clients on Vela/Plasma; controls detect an intentionally introduced problem in an isolated fixture |
| P3 Desktop observation | Automated launcher/window/desktop actions and one validated capture route per supported adapter | Verified actions/content; no mid-run dialogs or automatic backend substitution; insufficient capture cadence cannot produce fine-grained motion claims |
| P4 Scaling and overhead | B4 pattern/corpus and B5 full-session accounting with optional energy sources | Reproducible pixel/geometry evidence; no peripheral battery accepted as computer energy; collector/process boundaries verified |
| P5 Complete v1 | All five primary families, supported installed targets, archive comparison and operator documentation | One Start from a TTY uses the provisioned account, produces the complete offline report and restores initial service state; no provisioning/login prompts during measurement; Vela-version comparison uses archives across external updates; no side-by-side version manager or target build/install/update steps; every gap remains visible |

**Current acceptance:** P0 passed on 2026-10-22 in campaign
`706ab1a3d71144959665b0b5c89b1397`, within the initial-machine Vela/Plasma
scope described above. P1's common executor now carries phase/target/repetition
blocks through supervised sessions, fresh calibration, durable completion,
checkpoint/resume and offline reporting. `run --exercise` exposes this path
using the fixed `p1-instrument-controls-v1` workload in each phase slot:
three complete repetitions by default and one with `--fast`. The installed
launcher rebuilds the schedule from fixed phase/target/mode/seed selections;
it accepts no operator-provided script or executable. The worker verifies its
block binding before takeover and emits a receipt after the controls finish.
A block is accepted only when that receipt, raw controls, hardware conditions
and restoration all pass. Exports include the campaign journal and all
attempt/completion records, as well as per-block evidence.

CPU tests cover multiple phases, three-versus-one complete workloads, changed
receipt rejection, changed-environment refusal and interrupted-block resume
without rewriting accepted evidence. **P1 campaign-core acceptance passed on
2026-10-22 for the initial Vela/Plasma setup**, with these real-TTY exercises:

| Campaign | Mode | Selected phase slots | Accepted blocks | Verified exported files |
|---|---|---|---|---|
| `42889274d6e149358f189b23b884dd80` | `--fast` | motion, overhead | 4: one per phase/target | 178 |
| `fb409488498f48be8c3939df8af154e0` | normal | motion | 6: three per target | 265 |

Exports are under `benchmark-results/campaign-ID/`. Independent inspection
verified all 443 file hashes, reconstructed the schedules, checked phase
start/completion journals, recomputed raw acceptance audits, verified block
receipts and completion-marker hashes, and checked frozen hardware conditions
and restoration. Every block passed with no cleanup errors. Both campaigns
used the same installation, environment and resolved hardware profile; each
block retained the same fixed controls and sample counts. `--fast` changed
repetitions only. Neither campaign needed a retry, so they do not add a
real-TTY checkpoint/resume demonstration; that contract is covered by CPU
interruption/resume tests, alongside P0's physical crash/hang recovery evidence.

This closes P1's campaign-core gate within the initial-machine Vela/Plasma
scope. Exercise reports correctly record zero measurement blocks,
`exercised-not-measured` family coverage and P0 qualification `not-requested`:
these runs do not repeat P0's fault qualification. Actual scenario
implementations beyond the neutral client and complete-family comparisons
remain P3–P5 work.

### P2 implementation and acceptance scope

The `p2-neutral-client-v1` workload now runs through the same executor after
fresh instrument calibration. Each block keeps its own raw evidence and can
be resumed only under the same immutable selection, installation and hardware
conditions. `--fast` changes independent repetition count only, never sample
budgets, warm-up or controls:

| Phase | Implemented workload | Fixed measured sample and exclusions |
|---|---|---|
| responsiveness | Focused native Wayland SHM client, F24 response matched to presentation | Five warm-up actions, then 100 measured; all 105 actions validated. Report injection interval, injection-to-presentation lower/upper bounds, client receipt and receipt-to-presentation separately |
| motion | Native client animation with identified submissions and feedback | 30 warm-up submissions, then 600 measured; retain presented/discarded counts, p50/p95 intervals, maximum gap and gaps over 1.5 fixed refresh periods. These are client update gaps, not missed kernel vblanks |
| slow-apps | Responsive input/animation client alongside the 150 ms GPU acquire-fence producer, explicit then implicit synchronization | Each mode gets a fresh no-load input/animation control in the same block. Collect both absolute metrics and loaded-minus-control differences. CPU/GPU contention as dedicated workloads remains unimplemented |

The GPU producer uses the compositor's DRM device, reports Vulkan device and
queue identity, timestamps each compute job with GPU queries, verifies that
the fence is pending at commit, and records continued presented buffer IDs.
Its JSON mode uses a 1024×640 surface behind the 640×360 neutral client to
avoid complete occlusion. Qualification requires at least five presented jobs,
complete submitted/completed GPU work, observed production/presentation gaps
no longer than two seconds across the measurement window and a median achieved
GPU duration of 75–225 ms for the fixed 150 ms request. Calibration wall time
is retained separately and cannot stand in for achieved GPU execution time.
Timestamp support, device access or synchronization failures invalidate the
workload; no implicit substitution for explicit sync is allowed. Even a
compute-only queue does not establish absence of contention; reports retain
that limitation instead of attributing every effect solely to fence handling.

Physical runs require presentation feedback consistent with the frozen
fixed-refresh mode. Headless tests use an explicitly unqualified parser path;
that option is never exposed by the privileged campaign interface. Tests
exercise all 105 native input actions, all 630 animation submissions and both
GPU synchronization modes while a separate foreground client presents.
They validate protocols and real GPU timestamps, not physical display timing.
Real-TTY P2 `--fast` acceptance passed on the initial Vela/Plasma machine on
2026-10-22: campaign `20722bd65af2469ea242b323bcade07c` completed all six
responsiveness/motion/slow-apps blocks in about 7 minutes 33 seconds. The
offline review verified all 355 hashed export files, recomputed raw input,
animation and producer metrics, and checked calibration, workload receipts,
completion markers, event journals and hardware restoration for every block.
No cleanup errors or greeter recovery restarts occurred. Each standalone
animation sample presented all 600 measured frames without discards.
Achieved producer GPU-duration medians were 76.868–89.138 ms across the two
desktops and synchronization modes. These pass the declared 75–225 ms gate,
but are not evidence of an achieved 150 ms delay. Retain that distinction
when interpreting slow-app results. Normal three-repetition acceptance remains
deferred at the operator's request to proceed with P3; a single fast run does
not establish repeatability or a desktop ranking.

Offline reports audit raw scenarios again, keep per-block statistics separate,
and show median/range of independent block p50/p95 values. They do not pool
frames/actions as independent repetitions, report p99 from these samples or
claim statistical significance. Coverage is `partial-client-only`, including
when all selected P2 blocks finish: B1 still needs native shell response, B2
real window motion, and B3 desktop observation/dedicated contention workloads.
`full_suite_complete` and automated complete-family comparison remain false.
See the implementation guide for the P2 build, TTY run and resume commands.

### P3 implementation and acceptance scope

`observe` executes the separate fixed `p3-desktop-observation-v1` workload,
with independently selectable responsiveness/motion phases, one repetition
in fast mode or three in normal mode. It uses the common privileged campaign
and per-block session/calibration/restoration contracts. P2 archives and metric
identities remain unchanged. A missing scene tool fails before takeover.

Responsiveness observes one warm-up and five measured native launcher
open/close cycles, verifies return to baseline, searches and launches the
unique scene entry through the launcher, and requires both its launch receipt
and identified captured content. Show-desktop must hide the scene and restore
must reveal it. The disposable application entry exists before session launch;
the normal application corpus, shell effects and services remain enabled.

Motion presses the fixed native Wayland scene's separate drag strip, below
the encoded marker so a captured software cursor cannot cover its identity
bits, through persistent
uinput input and the standard system-move request. Twelve captures must show
the identified window at at least three positions with at least 80 physical
pixels of final displacement. This establishes coarse compositor movement,
not frame-level smoothness or missed physical refreshes.

Each capture sequence contains two pre-action and ten subsequent samples,
using the adapter's P0 route and a nominal 100 ms sleep between operations.
Keep physical-pixel PNGs, backend identity, request/receipt bounds, fixed
32×18 pixel fingerprints, marker positions and input intervals. Launcher
response requires at least 12 changed tiles (RGB L1 difference above 30 at
each grid sample) and settles within eight tiles; the passive/close control
must return within eight tiles. These
are provisional versioned state-detector tolerances, not image-quality scores.
Report only a conservative 0-to-first-observation response interval including
capture delay. A changed image alone is insufficient: actual launcher search
and scene launch are mandatory functional controls. Captures with another
geometry/backend, missing actions/content, rejected movement or failed
restoration invalidate the block. There is no automatic capture fallback.

The offline report links before/after images and re-audits the observations.
CPU rejection/export/schedule tests, native Vela scene capture and real window
dragging with the button held and cursor included, and a private KWin ScreenShot2
scene capture pass. Failed action analysis retains all collected PNGs, timestamps,
input intervals and the observed marker IDs. Real-TTY responsiveness passed for
Vela and Plasma in campaign `36c9f47c6ba841ff844ec5ddca803db8`, including launcher
search/launch and desktop hide/restore. That campaign rejected Vela's motion
block because the old grip placed the cursor over the marker; it remains failed.
With the corrected grip, both real-TTY motion blocks passed in fast campaign
`dc4fe800fca6426db3c08caabc698afb` on 2026-10-22. Each desktop produced 12
captures, ten identified positions and a verified 162-physical-pixel displacement.
All 123 exported evidence-file hashes, raw-derived motion metrics, instrument
controls, block receipts, completion markers and restoration checks were
independently verified. Both blocks restored conditions without cleanup errors
or a greeter recovery restart.

Together these campaigns establish initial-machine fast acceptance of the
implemented P3 actions on Vela and Plasma. They use different scene builds
because of the grip correction; they are separate functional acceptance evidence,
not one complete campaign or repeated performance-comparison evidence.
Normal three-repetition P3 acceptance has not been run. Coarse captures do not
establish frame-by-frame regularity or missed refreshes.
Alt+Tab/context menus, additional shell motion, desktop
switching and observed interaction under slow-producer load remain unimplemented.
The result is partial desktop coverage, not complete B1/B2/B3 or v1 acceptance.
See [P3 commands](../benchmarks/README.md#p3-native-desktop-observation).

### P4 implementation and acceptance scope

`run --phase scaling` and `run --phase overhead` implement the separate fixed
`p4-pattern-resources-v2` workload. Mixed P2/P4 selections retain each block's
own version/specification in the immutable schedule and receipt. Default
repetitions remain three; `--fast` changes only their count. Offline exports
recompute original pixel and process/counter evidence, never trust only the
saved aggregates. Neither phase claims complete family coverage.

Scaling now covers a native Wayland coordinate pattern at 100/125/150/175/200%
in opened, snapped-left, snapped-right, maximized and restored states. Each
state starts a fresh 401×301-logical-pixel pattern, records its configure,
preferred scale, viewport and actual buffer dimensions, injects the desktop's
fixed default shortcuts, and captures the original physical output. Window
actions are target-specific: Vela uses Meta+Up/Meta+Down to maximize/restore,
while Plasma uses its Meta+PageUp toggle twice for the restored case. Plasma's
Meta+Up tiles to the top and Meta+PageDown minimizes. Version 2 corrects the
shared mapping used in version 1; a changed campaign must start anew rather
than reuse a version 1 completion. Native maximized state and original-size
restoration remain mandatory. Output
configuration is read back. Before rendering, a recorded fixed pointer action
parks the cursor at the output's bottom-right corner to prevent contamination.
The decoder checks PNG CRCs, geometry, bounded
decompression and all declared interior pixels without resampling. The client
buffer establishes the expected rectangle; the oracle cannot shrink it to a
good-looking subregion. Record the eight-logical-pixel corner exclusion plus
pixel-grid allowance and at most two pixels of edge overhang. Overhanging
pixels are counted as offscreen rather than discarded. Nonzero pixel errors
are valid measured outcomes, separate from wrong/missing action identity,
scale, capture route or restoration, which invalidate the block. Scale 1 is
reapplied/read back on success and failure before ordinary session teardown.

Overhead omits presentation/input/capture calibration and keeps capture off
throughout. After ten seconds of startup settling, it measures 30 seconds
of idle, 1,800 native animation submissions, and a fixed synthetic local
SHA-256 workload (16,384 hashes of a fixed 1 MiB corpus). Source completion
must fall inside its sampled window. One-second samples keep PID/start-time
identity, service/workload cgroup membership, per-process PSS, disappeared
processes and inventory intervals across the whole reserved UID. Cgroup CPU
counters include processes that exit between samples. Report CPU seconds,
percent of one logical CPU, UID/workload-separated PSS and root-controller
CPU cost without automatic subtraction. PSS is a non-atomic observed-live-PID
inventory; retained intervals and gaps expose this limit. Shared system
services are not attributed. GPU and energy are explicitly unsupported for
these initial collectors. Missing permissions/counters, counter resets,
foreign UIDs/cgroups, sampling gaps and incomplete workloads invalidate data.

Development validation covers rejection, schedules, phase-specific no-capture
execution, raw audits/export, a complete isolated Vela 25-case controller run,
and a fractional pattern through private KWin's native capture with verified
scale restoration. A private KWin 25-case controller check also verifies its
effective/default shortcut mapping and reproduces top tiling before testing
maximize/restore through D-Bus action fixtures. Its virtual VRR policy and
pointer parking are fixture boundaries, not physical hardware qualification.
The Vela fixture uses virtual input rather than kernel
uinput and is headless; no physical P4 campaign is qualified yet. The new
pixel oracle exposed nonzero fractional-edge differences; these are retained
as measurements, not relaxed away to make the compositor pass.

The application corpus (Qt/GTK/browser/Xwayland), resource interaction
sequence, additional resource backends and real-TTY acceptance remain open.
Coverage is `partial-pattern-only` / `partial-session-resources`;
`full_suite_complete` and `performance_comparison_available` stay false.
See [P4 commands](../benchmarks/README.md#p4-scaling-pattern-and-session-resources).

### Upstream integration review (2026-11-02)

The benchmark branch integrates `main` through `cd5ff9f`, including the
`v0.1.0` release history. Installed Beta/Experimental artifacts and this source
checkout are different identities: an archive must describe the executable
actually run. The following decisions guide the remaining implementation;
P4's implemented partial scope is described above. The latest pointer-lock
and visible-resize-border fixes (`#189`, `#192`) also extend `vela-pattern`:
the benchmark metadata mode remains available, and pointer logging/locking
stay opt-in rather than changing the fixed P4 stimulus.

| Upstream implementation | Benchmark use | Decision and acceptance work |
|---|---|---|
| Deferred SHM readback, shared staging and queue/damage fixes (`#111`, `#156`, `#173`) | Reduce interference from P0/P3 captures; inspect capture latency and throughput | Integrated through the existing capture protocol. Repeat capture-off/on, known-delay and cadence controls on the exact installed candidate. Existing coarse qualification does not become fine-grained motion qualification automatically. |
| `vela-capture-probe` and `functional-capture` | Instrument diagnostics for latency, new/reused buffers, concurrency, idle waiting and queue pressure | Reuse the tests and workload ideas before extending observation. The probe currently exports aggregate percentiles, not the raw per-request series required for an audited benchmark. Its completion latency includes source-damage waiting; FPS is capture throughput, not monitor FPS. |
| `vela-screen-record` and `scripts/recording_core.py` | Optional raw video evidence and deterministic shell-action fixtures | Reuse small components behind a declared adapter, after recording per-request bounds, timestamp origin, capture failures and workload identity. Keep raw frames/times. The current recorder falls back to receipt-time `CLOCK_MONOTONIC` when a presentation timestamp is absent and retries failures; those facts must become explicit evidence before timing use. Preserve P3's current route until the replacement is qualified. |
| Before/after and promo scenes | Design fixed launcher, maximize, drag and quick-settings workloads; attach optional illustrations | Reuse action/readiness patterns. The current headless/private-services orchestration, synthetic fixtures, optional speed/scale changes, visual alignment and fixed-rate video export serve demonstrations. They cannot replace physical full-session runs or supply primary latency/regularity metrics. Builds/cherry-picks stay outside benchmark v1. |
| Cooperative `vela-slowgpu` dispatches and portable linear DMA-BUF allocation (`#124`, `#126`) | Improve portability and separate delayed acquire from a single long unpreemptible dispatch | Integrated, preserving JSON, full-job GPU timestamps and bounded execution. Calibration records dispatch count; 150 ms requests use 50 dispatches by default. `--monolithic` remains a developer diagnostic, outside the current fixed campaign selection. Old single-dispatch archives and cooperative runs are different stimuli, even with equal requested duration. Do not pool them or attribute the difference solely to a desktop version. |
| X11 native-pixel coordinates/RandR/DPI and `test_x11_native.py` (`#167`) | P4 Xwayland scaling oracle for geometry, native pixels and input mapping | Reuse the checkerboard, geometry and coordinate assertions alongside the existing Wayland pattern. Compare default Xwayland policy separately from native Wayland; no forced DPI policy or font-quality ranking. Headless source tests do not establish the installed DE's physical-output result. |
| Renderer identity checks and strict AMD/NVIDIA GPU workflows | Verify real GPU selection and prevent a software-renderer/skip result from appearing as hardware coverage | Reuse selected-node/vendor checks and exact-candidate provenance. These workflows validate Vela on GPU-backed headless outputs, not a Vela/Plasma physical TTY campaign. Dispatch and hardware qualification remain separate from hosted CPU CI. |
| Beta/Experimental packages and release branches | P5 comparisons of installed Vela releases | Extend discovery to distinguish `vela` and `vela-git`, artifact/channel/source metadata and component hashes. Never derive the installed binary's commit from this checkout or invoke the updater during a measurement. Archive comparison remains across external installations. |
| Native Bluetooth and update daemons | P4 full-session resource inventory | Include target services started through the supervisor, D-Bus or user manager, including new helpers outside the compositor child tree. Record their identity/activity and common network/cache conditions. Keep default services active and collector/client costs separate; no live updater or device-pairing action is needed to discover them. |

Implementation order: first requalify the existing instruments against the
current installed target, then accept the initial P4 pattern and UID-wide
resource workloads on physical outputs. Extend the P4 corpus using the
existing sharpness and X11 oracles.
Persistent high-cadence capture and further shell scenes are a separate P3
extension with a versioned workload, raw timing records, capture-off/on
controls and a compatible Plasma adapter. Vela-private `state` counters
(`readback`, output `frames`/`missed`, scanout) may explain results in a separate
diagnostic channel; they are not common cross-desktop primary metrics.

The review does not transfer AMD/NVIDIA CI or historical TTY acceptance to
this integration. Physical P0/P2/P3 requalification remains outstanding.

The current `p0-instruments-v1` qualification fixes 20 input actions in
capture-off/on conditions, 12 captures, five known-delay input actions,
120 baseline/delayed animation submissions and two independent calibration
blocks per target. Known 40 ms input delay must shift the median by at least
25 ms; A/A median spread must stay within max(5 ms, two refresh periods).
These tolerances gate instruments only, not desktop performance comparisons.
The capture capability is explicitly coarse; it cannot satisfy P3's future
fine-grained shell/window motion metrics. See the implementation guide for
fault deadlines, hardware prerequisites, checkpoint/resume and export.

P0 is a technical gate: a manual session switch or a capture method that
requires intervention does not count as passing it. Do not build an
apparently automated suite whose central transition still needs the user.
An unsupported secondary metric must not be used to hide a missing primary
scenario. Start with a small default campaign; derive its per-block sample
budgets, stabilization limits and detector tolerances from the archived pilot
before freezing measurement version 1.

CPU-only CI covers campaign validation, estimation arithmetic, ordering,
state recovery, timeout handling, statistical/report logic and adapter
fixtures. It also checks fixed-order phase filtering and prerequisites,
fresh calibration on run/resume, block completion/restart semantics,
fast-mode repetition counts and unchanged scenarios, offline JSON/JSONL
reporting and subset/full-suite coverage. Verify that a failing capture
backend cannot silently select another route.
Real hardware is required for PAM/logind/VT operation, output
configuration, input handling, capture cadence and the five actual families.
Follow [hardware coverage](hardware.md): one successful AMD machine does
not establish Intel/NVIDIA or hybrid-GPU correctness. No hardware-dependent
pass is inferred from a mocked test or a headless CI run.

## 11. P0 findings and remaining validation questions

The product requirements in §1 do not need another choice from the operator.
The P0 campaign answered the PAM/session/isolation/recovery questions below
for the initial Vela/Plasma setup and validated coarse capture with its declared
controls. Recheck those contracts for each additional adapter or platform.
Fine-grained motion, performance sample budgets and broader hardware/energy
coverage remain open for the later stages:

- Does the benchmark account's normal PAM/logind session provide the expected
  desktop/device policy, native runtime, user bus and full service activation?
  Does teardown remove all target processes/runtime state before the next
  profile is created, including indirectly activated helpers?
- Can the PAM/logind launcher start and cleanly end each full default session
  from a TTY, restore the originating environment and preserve the previous
  display-manager state without a greeter, autologin edits, leaked services
  or prompts, including on repeated fresh-profile launches?
- Which capture path on each desktop is unattended, preserves physical
  pixels, and has sufficient timing resolution for the requested metric?
- How large is capture/collector interference, and which native UI motion
  claims remain supportable at high refresh rates?
- What sample counts and independent blocks support useful tail estimates,
  and what detector tolerances distinguish intentional easing from stalls?
- Which measurements and optional energy/GPU backends work on additional
  machines without changing scenario definitions or metric meaning?

If a proposed mechanism fails, update this design and the capability matrix.
Do not silently weaken unattended execution, substitute internal Vela
counters for cross-desktop evidence, or claim hardware validation that has
not happened.
