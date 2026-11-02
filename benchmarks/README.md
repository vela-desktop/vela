# Desktop benchmark implementation

This implements the first parts of the [suite design](../docs/benchmark-suite.md).
It provides read-only discovery/planning, preflight archives, a supervised
TTY session launcher, native instruments, automated P0 acceptance cases and
P1 phase/target/repetition execution with checkpoint/resume. **The five
performance families remain incomplete.** P2 now provides native-client input,
animation and paired GPU-producer measurements; their six-block real-TTY fast
acceptance passed on the initial machine, with normal repetition acceptance
deferred by the operator. P3's implemented desktop actions also passed fast
acceptance across separate responsiveness and motion campaigns. P0 passed on
the initial machine for Vela and Plasma Wayland
on 2026-10-22: all eight real-TTY calibration/crash/hang blocks passed with
verified restoration. This qualifies the declared instruments and recovery
paths on that setup; it provides no desktop ranking or broader hardware claim.
P1 campaign-core acceptance also passed on that setup: a four-block
multi-phase `--fast` exercise and a six-block normal exercise. Headless tests
do not establish real-TTY qualification.

## Try the available commands

The coordinator needs Python **3.11 or newer**, with no pip dependencies.
Run from the repository root; it does not need a compositor build, root or a
TTY for host checks and planning:

```sh
scripts/vela-bench phases
scripts/vela-bench check
scripts/vela-bench plan benchmarks/campaigns/default.toml
scripts/vela-bench plan --phase overhead --target vela --fast
scripts/vela-bench run --phase check
```

The last command creates a private, uniquely named directory below
`benchmark-results/` and prints its path. Pass that **directory path** to:

```sh
scripts/vela-bench status benchmark-results/RUN_ID
scripts/vela-bench report benchmark-results/RUN_ID
scripts/vela-bench compare benchmark-results/RUN_A benchmark-results/RUN_B
```

`--output PATH` on `run` changes the parent directory. Each invocation
creates a new archive. `--json` produces machine-readable output for every
archive/planning command. `compare` currently reports metadata differences only;
it cannot produce performance deltas or regression claims.

Normal planning schedules three repetitions per measurement family/target;
`--fast` schedules one with the same block budgets and prerequisites.
Phase flag order does not change execution order. Targets rotate between
repetitions using the persisted seed. This is rotation, not a statistical
guarantee of fully balanced order in every subset.

Planning allowances and the native-resolution/highest-common-fixed-refresh
policy are provisional. Discovery does not resolve the active primary
monitor, scale, refresh or brightness from sysfs, and does not apply them.
The explicit controlled-session adapters below resolve and verify those conditions. The printed duration
range is uncalibrated; the measurement scenario revision is still `draft-1`.

Discovery inspects normal `vela.desktop`, `plasma.desktop` and `gnome.desktop`
Wayland session entries, without executing their `Exec` commands. A discovered
entry is not a qualified adapter. Explicit missing targets are errors. Debug
session variants are excluded. Entry/executable hashes, available compositor/shell component hashes and selected package
versions identify installed artifacts; a checkout commit is never attributed
to an installed executable. Unsupported package managers and unverified
battery scopes remain explicit gaps.

## P0 account and session pilot

Preview without changing the host:

```sh
scripts/vela-bench setup --dry-run
scripts/vela-bench session-test --dry-run --target vela --target plasma
scripts/vela-bench uninstall --dry-run
```

Actual setup is an explicit administrator operation:

```sh
sudo scripts/vela-bench setup
```

The current prototype requires systemd/logind, `/usr/bin/python3` 3.11+ and
an Arch-style `system-login` PAM stack containing `pam_systemd`. It rejects
other stacks; Debian/Fedora policy support is pending. Setup installs no
packages and grants no sudoers/polkit permissions. Its resources are:

| Resource | Purpose |
|---|---|
| Normal `vela-benchmark` account/primary group | Stable non-root UID, locked password, no supplementary groups, lingering disabled |
| `/var/lib/vela-benchmark/home` | Canonical NSS home, recreated between targets before PAM/user-manager activation |
| `/var/lib/vela-benchmark/tmp`, `var-tmp` | Shared disposable temporary directories for session and user-manager services |
| `/var/lib/vela-benchmark/mail` | Root-owned directory containing any optional mailbox created by `useradd`; removed at uninstall |
| `/var/lib/vela-benchmark/setup.json` | Root-only account identity, installation tag and installed-file hashes |
| `/usr/local/libexec/vela-benchmark/` | Root-owned launcher and a fixed list of Python modules; no imports from the operator's checkout during a pilot |
| `/etc/pam.d/vela-benchmark` | Account/session integration through `system-login`; password authentication is denied |
| `/etc/systemd/system/user@UID.service.d/70-vela-benchmark.conf` | Filesystem isolation for this UID's user manager and indirectly activated services |
| `/etc/systemd/system/vela-benchmark-pilot.service` | Fixed lifecycle/calibration worker, 600 s runtime deadline and 300 s recovery deadline |
| `/etc/systemd/system/vela-benchmark-qualification.service` | Fixed fault worker, 90 s runtime deadline and the same supervised recovery |

The same home/runtime policy applies to both process trees: mask personal
homes and other users' runtime directories, bind the native benchmark runtime,
and share its disposable temporary directories. Nonstandard personal home
paths from normal-user NSS entries are included, or rejected if they conflict
with system paths needed by the runner. Filesystem policy and normal service
activation still require real-session validation. No device-group grants,
autologin configuration or remote-login credentials are installed.

Setup refuses existing unowned accounts, paths and units. A completed recorded
setup is idempotent. A failure before account creation can be retried with
`setup`: it verifies the exact root-owned bootstrap record, absent account
and group, no installed files and no unexpected bootstrap contents before
continuing. This also handles the initial unsupported `CREATE_MAIL_SPOOL`
override error. Other interrupted provisioning remains partial and requires
inspection; uncertain resources are never adopted or deleted automatically.
Optional mailbox creation is redirected into the owned directory with the
supported `MAIL_DIR` override; host defaults are unchanged.
Rerunning `setup` refreshes the owned installed launcher code from this
checkout while retaining the same account, PAM/isolation configuration and evidence:

```sh
sudo scripts/vela-bench setup
```

This requires a stopped/restored pilot and no remaining benchmark UID
processes/runtime. It verifies installed hashes before updating, publishes
each file atomically and journals old/new identities. An interrupted update
can resume by repeating the command with the same source. Changed unrecorded
files are refused. Owned worker supervision units are also upgraded in place, including their
recovery deadline and the new fault unit; systemd is reloaded when needed.
Unrelated PAM/isolation-policy changes still require uninstall and setup. Successful setup prints `Launcher code: up to date with
this checkout`; `--json` prints the full installation record instead.
`setup --update` remains available as an update-only command requiring an
existing setup; `setup --update --dry-run` previews that operation.
`session-test` refuses to execute installed code that differs from the
checkout and tells the operator to rerun `setup`. Recovery remains available
through the installed launcher even when the checkout differs.

For the actual pilot, **log out of graphical user sessions and log into a
local TTY on seat0**. Merely switching away from an open desktop is insufficient.
The login greeter may remain open: the pilot stops and restores the display
manager itself. Sessions that logind reports as `State=closing` and
`Active=no` have already logged out and do not block the lifecycle pilot,
even if background remnants keep their logind records alive. The pilot does
not terminate these personal background processes or establish a clean
performance baseline. An inactive `online` desktop on another VT still
blocks takeover; errors list the specific blocking session identities.
`sudo` may wrap that console in `/dev/pts/N` through its default `use_pty`
behavior. The launcher resolves its own caller session with logind `self`,
requires that session to be local/active/TTY on seat0, and checks the physical
Linux VT device and active VT with kernel ioctls. Input/output remain on
sudo's PTY. A graphical or remote terminal is still refused; environment
hints such as `SUDO_TTY` cannot authorize takeover. No sudoers change is needed.
From the checkout run:

```sh
sudo scripts/vela-bench session-test --target vela --target plasma --seconds 30
```

It prints the selected targets, estimated duration, effects and cancellation
command, requires typing `START`, then counts down for five seconds. `--yes`
explicitly acknowledges that notice; the TTY and occupancy checks remain.
Default target discovery selects the installed normal Vela/Plasma entries;
`--seconds` controls a 5–120 s observation interval per desktop. GNOME is not
enabled in the pilot. No operator login or target selection is requested
between desktops.

To add automatic **presentation diagnostics** to that same session cycle,
build and install the native probe once, then use `--probe`:

```sh
cmake --build build --target vela-bench-probe
sudo scripts/vela-bench setup --probe build/tools/vela-bench-probe
sudo scripts/vela-bench session-test --target vela --target plasma --seconds 30 --probe
```

Replace `build` with your configured build directory (for this checkout,
`build-native-displays` is available). Setup copies the native ELF into its
root-owned code directory and records its hash; it never executes the binary
as root. Existing owned installations gain the new diagnostic module and
optional binary without recreating the account or changing PAM isolation.
Repeat the same setup command after rebuilding the probe. Code-only setup
without `--probe PATH` preserves an already installed binary.

After each desktop's observation interval, two visible animated-window
workloads run automatically: 120 submissions without injected delay, and
120 with a known 40 ms client CPU sleep per submission. Each runs as the
benchmark UID through its user manager, using that desktop's environment
and filesystem view. A PID 1 deadline and a controller timeout bound it.
Missing protocols, timeouts and malformed/partial feedback produce failed
samples; the remaining scenarios/desktops are attempted while the session
remains healthy. Any failed requested diagnostic prevents overall success,
and cleanup still runs. The explicit target list survives JSON key sorting.

The terminal prints each target/scenario and final presented/discarded
counts plus submission-to-presentation p95. After restoration, the coordinator
automatically exports a new `benchmark-results/pilot-RUN_ID/` directory, with
offline `report.html` and `summary.json`; `session-test --output PARENT`
chooses another parent. Preflight failures do not export an older run.
`sudo scripts/vela-bench pilot-report --output ANOTHER_PARENT` separately
exports the latest run again if needed. Reusing an export directory is
refused. Exported files belong to
the invoking sudo operator; privileged source evidence remains unchanged.
The JSON retains raw successful feedback, binary identities, clock metadata,
counts, flag values, submission-to-presentation and presentation-interval
distributions, and observed client sleep. Reports revalidate the raw stream
before using saved statistics. Failed and unexecuted scenarios stay visible.

These are **unqualified client protocol diagnostics**, not completed B1/B2
benchmarks or a desktop ranking. With `--probe` alone there is no input or
optical measurement, controlled monitor/power state, campaign repetition or
warm-up qualification. Use `qualify` for the full instrument acceptance report.
The CPU sleep is an instrument control rather than the slow-GPU B3 workload.
Without `--probe`, `session-test` continues to run only lifecycle checks.

The root worker lives outside the reserved UID's teardown scope. PID 1 invokes
cleanup through `ExecStopPost`, including after timeout or worker failure.
The pilot stops an initially active display manager, reserves a free VT by
temporarily masking both exact `getty@ttyN.service` and `autovt@ttyN.service`
names, while preserving existing administrator masks, starts complete
installed session entries as the benchmark UID with `PAMName` and a controlling
TTY, then verifies active local Wayland/logind state, native runtime/bus and
the graphical session target. It checks a synthetic world-readable fixture
under `/home` is hidden in both the session leader's and user manager's mount
views. It does not inspect personal documents.

Mask files and inactive/masked unit state are verified before switching VT.
An alias can report its canonical unit's `UnitFileState` without having its
own mask; that property alone does not reserve the VT. Occupied terminals
remain refused (`StandardInput=tty-fail`).

Teardown stops the owned transient session unit, terminates the reserved UID,
stops its user manager/runtime and verifies that no PID/runtime remains before
resetting its canonical home. Changed NSS identity, extra groups, unlocked
password, unowned unit, unsafe symlink or mounted profile prevents destructive
operations. A process outside the original session cgroup cannot count as
cleaned up simply because the launcher exited. The initial display-manager
state, owned getty masks and originating VT are restored and checked. Failures
remain visible and prevent a successful result. After reboot, recovery does
not apply stale VT/display-manager settings to the new login environment.

Return to the originating TTY and press Ctrl+C to cancel, or use the printed
`sudo systemctl stop vela-benchmark-pilot.service` command. To retry restoration
after the unit has stopped:

```sh
sudo scripts/vela-bench recover
```

Root-only pilot state, event journal and per-target verification evidence live
under `/var/lib/vela-benchmark/pilots/RUN_ID/`. `active.json` identifies the latest
pilot. Session output is in the journal for `vela-benchmark-session.service`
and `vela-benchmark-pilot.service`. Failed launches also save
`TARGET-launch-failure.json` with exit properties and a bounded session journal,
including failures before PID 1 opens the session's output file. A failed pilot
keeps its original error rather than requiring a workload completion receipt.
These are lifecycle diagnostics. When requested,
`TARGET-SCENARIO.jsonl`, `.stderr` and `.json` preserve probe evidence here;
each profile reset also creates private sticky `.X11-unix` and `.ICE-unix`
directories so Xwayland/ICE can create their standard sockets.

Passing the lifecycle pilot establishes only its stated lifecycle/namespace
checks. Full P0 acceptance, including the following hardware controls,
input/capture and crash/hang recovery, passed on the initial Vela/Plasma machine
in real-TTY campaign `706ab1a3d71144959665b0b5c89b1397` on 2026-10-22.
Its export is `benchmark-results/campaign-706ab1a3d71144959665b0b5c89b1397/`.
All 197 exported evidence-file hashes and all eight raw acceptance audits were
independently checked. Each block restored hardware with no cleanup errors.
The capture qualification covers coarse state observation, not fine-grained
motion. Additional adapters and platforms require separate acceptance runs.

## Automated P0 qualification and P1 calibration

Build and install the four fixed instruments (replace the build directory
if yours differs):

```sh
cmake --build build-native-displays --target vela-bench-probe vela-bench-input vela-bench-desktop vela-screen-dump
sudo scripts/vela-bench setup --tools-dir build-native-displays/tools
scripts/vela-bench qualify --dry-run --target vela --target plasma
```

The input helper requires libevdev; the Plasma output/capture adapter requires
Qt 6 Core/Gui/DBus and KF6Screen. CMake omits the corresponding optional target
if its development dependency is absent. Runtime requires `/dev/uinput`,
`/usr/bin/wlr-randr` for Vela, `powerprofilesctl` with a real performance
profile, and either `ddcutil` VCP 10 access for the primary external display
or one verified eDP backlight. Setup installs no packages or permission rules.
DDC reads and writes explicitly use `--noconfig`, `--disable-dynamic-sleep`
and `--disable-capabilities-cache`. The root controller intentionally has no
operator `HOME`: it must neither depend on personal ddcutil configuration/
caches nor fail initializing a cache filename in the reduced environment.
The same fixed options apply to snapshot, control, read-back and recovery.
After a VT/link transition, brightness reads allow up to four attempts under
one eight-second deadline for DDC device/communication failures or timeouts.
The backend, bus and VCP code stay fixed; successful snapshots retain every
read attempt. Writes are not automatically retried by this readiness loop.
Syntax errors and signaled processes fail immediately. For a failed host
command in the supervised worker, the pilot retains bounded stdout, stderr, exit code and timeout details
in `failed-operation.json`, including read attempts, and exports that file.
This matters because ddcutil reports some failures on stdout, not stderr.
The root input helper emits only F24 through a temporary kernel uinput device;
no arbitrary-key or command interface is installed.

The neutral probe is native Wayland on both desktops: it creates an
`xdg-shell` toplevel with `wl_shm` buffers and requests `wp_presentation`
feedback. It does not use XWayland. New probe metadata records
`display_protocol=wayland`, `window_protocol=xdg-shell` and
`buffer_backend=wl_shm`; its title also identifies native Wayland.

Log out of graphical user sessions, enter a local TTY on seat0, then run:

```sh
sudo scripts/vela-bench qualify --target vela --target plasma
```

After one `START`, this runs **eight independent blocks**: two fresh
calibrations for each desktop, then a worker crash and a hung-worker/frozen
benchmark-UID recovery for each. Target order rotates between calibration
passes using `--seed` (default 0). Estimated duration is provisionally 8–24
minutes for both targets; actual budgets remain 600 seconds of worker runtime
plus 300 seconds of recovery per ordinary block. Fault workers have a fixed
90-second runtime limit. There are no intermediate logins or prompts.
Use `--case calibration`, `--case crash` or `--case hang` for a focused check;
an explicit subset cannot establish all P0 acceptance evidence.

After the single `START`, controlled runs briefly activate the **declared
Wayland greeter's VT** and snapshot its outputs before stopping it. A DRM
greeter can announce no outputs while the operator's TTY is active; querying
it there is not a usable baseline. The supervised worker journals restoration
intent before switching, verifies the active VT, and waits a bounded interval
for connected-output announcements. Acquisition failure returns to the
operator's TTY through `ExecStopPost`, including after worker interruption.
Pre-Start planning does not switch VTs or probe an inactive greeter's outputs.
Restoration likewise activates the new greeter before querying its outputs.
The initial adapter supports Plasma Login/SDDM on Arch-style
PAM only. Plasma Login runs KWin and the greeter in its own systemd user
manager, outside the greeter's logind session scope. The runner queries that
UID's native user-manager environment after dropping privileges, verifies
runtime/socket ownership and forwards only the required display variables.
When direct `kwin_wayland` has not exported `WAYLAND_DISPLAY`, the native
unprivileged adapter matches a live socket's `SO_PEERCRED` PID/UID to the
`org.kde.KWin` bus owner. It rejects missing/ambiguous matches rather than
assuming `wayland-0`. This lookup does not modify the greeter's environment.
Older SDDM private-session environments remain supported under the exact
greeter session scope. A personal account cannot supply either environment.
Exactly one primary must be resolvable. The runner enables only
that monitor at its native preferred geometry, highest advertised fixed
refresh, scale 1, normal rotation, VRR off, hardware brightness 100% and the
performance power profile. Every target must support that exact frozen mode;
there is no lower-mode or software-dimming fallback. Connected monitor/EDID
identities and power-source state are checked. Recovery remaps mode IDs,
restores output layout/primary, hardware brightness and power profile with
read-back, then returns to the original TTY. It attempts brightness and power
restoration even if restoring outputs fails. A replacement/hotplugged monitor
cannot silently receive the old monitor's layout/brightness writes.

Short cases can exhaust the login manager's configured start budget even
after clean stops. Recovery records the service state before starting it. If
that state is `inactive`/`success` and this start returns `start-limit-hit`,
it journals and performs one targeted `reset-failed display-manager.service`
and one retry. It does not alter rate-limit configuration, reset pre-existing
failures or retry other errors. The decision is exported in
`display-manager-recovery.json`. Repeated recovery cannot keep resetting it.

Session teardown also waits for the fixed owned transient session unit to
unload. A forced stop after freezing the benchmark UID can leave this unit
in `failed/timeout` even after all its processes are gone. After verifying
ownership, stopped state and UID/runtime teardown, the runner clears only
that unit's failed state and waits up to five seconds for collection. It
does not clear or stop a foreign unit. The original stop outcome is saved in
`*-session-stop.json`, captured before reset; inability to unload remains a cleanup
failure. The next desktop cannot start until the name is available again.

Each calibration records:

- 120 baseline and 120 known-40-ms-sleep presentation submissions.
- 20 software-injected F24 actions with capture off, then 20 with capture on;
  injection syscall bounds, client receipts and reported presentation share
  a verified `CLOCK_MONOTONIC` domain. One warm-up submission is excluded.
- Twelve physical-pixel PNG captures with decoded visible frame IDs and
  request/receipt bounds. Vela uses `ext-image-copy-capture-v1`; Plasma uses
  KWin ScreenShot2. No portal prompt or alternate-backend retry is used.
- Five actions with a known 40 ms client delay, which must shift median
  measured response by at least 25 ms. These are sensitivity controls, not
  meaningful p99 samples or a slow-GPU benchmark.
- CPU deltas for the entire reserved-UID cgroup in each condition and separate
  root-coordinator CPU/wall/RSS observations. These scopes remain explicit;
  they are not the B5 steady-state benchmark.

Capture is sampled approximately every 100 ms **plus capture/encoding cost**;
actual intervals are archived. Its accepted capability is coarse output-state
observation only. Fine-grained desktop motion, physical key/USB latency,
optical latency and panel response remain unsupported. A/A compares the two
independent input medians with a versioned tolerance of the greater of 5 ms
and two refresh periods. This is an instrument feasibility gate, not a
statistical desktop improvement/regression detector.
The prototype bounds captures to 16,777,216 pixels, 8192 pixels per dimension
and 12 MiB per compressed PNG; exceeding these budgets is an explicit failure.

Fault cases persist their intent and a live uinput device before killing
only the worker, or freezing exact benchmark-UID processes via pidfds.
PID 1's fixed worker deadline and `ExecStopPost` must restore the machine;
the audit requires its `signal`/`timeout` result, complete hardware restoration
and disappearance of the temporary input device. Root/account/session
mutations are mocked in CPU tests; the actual fault path is not yet qualified
on this hardware.

The coordinator records immutable context, target/helper identities and block
completions under `/var/lib/vela-benchmark/campaigns/CAMPAIGN_ID/`. `plan.json`
freezes the display policy and installation/host context before Start;
`conditions.json` freezes the resolved primary/mode/monitor identities and
power-source conditions from the first supervised active-VT acquisition,
before stopping the greeter or launching a desktop. The write-once resolution
is fingerprint-bound to the plan, checked on resume and checked in each new
worker before any controlled setting changes. Each attempt
has a distinct pilot ID and fresh session/profile/calibration. No failed
block is retried automatically. After a failure, use the printed ID:

```sh
sudo scripts/vela-bench qualify --target vela --target plasma --resume CAMPAIGN_ID
```

Keep the same targets, case, seed, installed versions and host conditions.
Completed blocks are hash-verified and preserved; incomplete blocks start
again with new disposable state. Changing tools/desktops or editing completed
evidence refuses resume. Root pilot evidence and previous attempts remain
local. `recover` retries restoration of an interrupted pilot before another
campaign can take over.

P1 also executes the standalone calibration phase once per target:

```sh
sudo scripts/vela-bench run --phase calibrate --target vela --target plasma --fast
```

`--fast` is recorded but does not shorten calibration or duplicate its work;
three-versus-one repetitions apply to the future measurement families. Resume
this selection with the same `run` arguments plus `--resume CAMPAIGN_ID`.
Ordinary responsiveness/motion/slow-apps requests now execute the P2 client
workloads below. Scaling/overhead execute the partial P4 workloads described below.

## P2 neutral-client measurements

Build the native clients and install the updated launcher/instruments (the
existing build must enable `VELA_BUILD_TOOLS` and
`VELA_BUILD_GPU_TEST_TOOLS`; the latter requires Vulkan/GBM/libdrm and `glslc`):

```sh
cmake --build build-native-displays --target vela-bench-probe vela-bench-input vela-bench-desktop vela-screen-dump vela-slowgpu
sudo scripts/vela-bench setup --tools-dir build-native-displays/tools
scripts/vela-bench plan --phase responsiveness --phase motion --phase slow-apps --target vela --target plasma --fast
```

After logging out of the personal desktop, from a real local TTY:

```sh
sudo scripts/vela-bench run --phase responsiveness --phase motion --phase slow-apps --target vela --target plasma --fast
```

One START executes six blocks. Without `--fast`, the same work runs in three
fresh repetitions per phase/target (18 blocks). Estimates remain provisional;
each block has the existing 600 s worker and 300 s recovery deadlines.
Select any one of those phases independently. The run performs fresh P0
instrument controls before each measurement block. `--exercise` is still
available but runs instrument controls only; omit it for actual P2 samples.
Resume with the same run arguments plus `--resume CAMPAIGN_ID`. Setup/tool
updates change fingerprints and require a new campaign.

If a previous run reports `restoration: failed`, update the installed launcher
and tools, then run `sudo scripts/vela-bench recover` from the TTY before starting
a new campaign. Do not start a personal desktop during recovery. A native
KScreen configuration rejection can trigger one recorded greeter restart;
the original monitor identities, modes, layout, brightness and power profile
must still pass verification. Both attempts and full bounded operation errors
are exported in `conditions-recovery.json`. This is a recovery measure, not a
fix for the underlying KWin/amdgpu RA24 format incompatibility; another rejection
still stops the campaign.

The versioned `p2-neutral-client-v1` workload fixes these samples for both modes:

- Input: five warm-up actions plus 100 measured F24 responses, one outstanding
  action at a time. Save and validate all 105 actions and matching feedback;
  separate injection uncertainty, software presentation bounds, client receipt
  and receipt-to-presentation. Failed/lost/discarded actions invalidate the
  sample and remain visible in raw output and block failures.
- Animation: 30 warm-up submissions plus 600 measured frames; report discarded
  frames, interval/latency p50 and p95, maximum gap and gaps over 1.5 refresh
  periods. The measured interval is active client animation, not idle time or
  native desktop/window motion. The gap count is not a kernel vblank counter.
- Slow producer: explicit and implicit GPU acquire-fence cases, each with its
  own new no-load input/animation control in the same block. Report absolute
  results and paired differences. The producer exposes device/queue identity,
  per-job GPU timestamps, a pending fence at commit, completed job counts and
  continued presentation while the foreground client runs. Its larger window
  keeps a visible region around the foreground probe. GPU duration must have
  a median of 75–225 ms for the fixed 150 ms request; gaps in production or
  visible updates over two seconds invalidate coverage. Dedicated CPU/GPU
  contention cases remain unavailable, and absence of resource contention is
  never inferred from use of a compute-only queue.

Missing GPU timestamps, permissions or synchronization support fail that
workload explicitly. The suite adds no permission rule, sync-mode fallback or
special desktop configuration. Snapshot/restore and instrument requirements
remain those already documented above; `slow-apps` additionally requires the
installed `vela-slowgpu` before any block takes over the machine.

Reports record `execution_mode=measure`, the accepted measurement-block count,
per-scenario raw-derived results and median/range of independent block p50/p95
statistics. They do not pool actions/frames as repetitions, infer p99 or claim
significance. Offline `status`/`report` revalidate raw P2 data and aggregates.
Family coverage remains `partial-client-only`; full-suite completion and
complete-family performance comparison stay false. Native shell response,
window movement is separately observed by P3; P4 implements the partial
scaling-pattern and session-resource workloads below.

CPU and isolated native Wayland/GPU tests cover this implementation. A
headless backend cannot qualify physical presentation timing; its test-only
parser option is not available to privileged campaigns. The six-block real-TTY
fast campaign `20722bd65af2469ea242b323bcade07c` passed on the initial
Vela/Plasma machine on 2026-10-22. All 355 exported evidence files and the
raw-derived metrics, instrument controls and restoration checks were verified.
Achieved producer GPU-duration medians were about 77–89 ms, within the declared
75–225 ms gate; they must not be presented as achieved 150 ms loads.
The operator deferred the normal three-repetition acceptance run and chose to
continue with P3. The archived fast acceptance remains the available physical
evidence; omitting `--fast` would run 18 blocks.

## P3 native desktop observation

P3 has its own fixed observation workload, `p3-desktop-observation-v1`, and
keeps its results separate from P2 client timings. It reuses the same
supervised campaign, account isolation, calibration, condition verification,
checkpoint/resume, restoration and offline export. It does not change the
desktops' effects, shortcuts or animation settings.

Build/install the scene alongside the existing instruments. Inspect the plan
from the ordinary desktop without changing the host:

```sh
cmake --build build-native-displays --target vela-bench-scene vela-bench-input vela-bench-desktop vela-bench-probe vela-screen-dump
sudo scripts/vela-bench setup --tools-dir build-native-displays/tools
scripts/vela-bench observe --dry-run --target vela --target plasma --fast
```

After logout, run from the local TTY:

```sh
sudo scripts/vela-bench observe --target vela --target plasma --fast
```

One START runs four blocks: responsiveness and motion on each desktop. Normal
mode runs 12 blocks with three independent repetitions. Select either phase
with `--phase responsiveness` or `--phase motion`; resume with the identical
arguments plus `--resume CAMPAIGN_ID`. Code/tool changes require a new campaign.

The responsiveness block installs one fixed `velabenchscene` application entry
in the disposable profile before starting the desktop. It opens/closes the
native launcher once for warm-up and five times for measured observations.
Each action has two baseline captures followed by ten captures through the
declared backend, with a nominal 100 ms sleep between capture operations.
A changed and settled 32×18 pixel fingerprint establishes a coarse response;
the report gives a conservative 0-to-first-observation interval including
capture delay, without a p95/p99 or optical latency claim. Closing must return
to the baseline and the known scene must remain visible. The script then
searches the unique fixture name through the launcher, presses Enter and
requires both a launch receipt and its distinct visible marker. It closes
that window and verifies show-desktop and restore-desktop through marker
disappearance/reappearance. A different effective keyboard layout or shortcut
that prevents these fixed actions fails verification; no alternative action
backend is silently selected.

The motion block uses a native Wayland 640×360 frameless scene with a fixed
drag strip below its identity marker. A persistent kernel uinput keyboard/absolute pointer presses the
strip, requests the standard system move, traverses a bounded path and
releases. Captures must identify the same marker at at least three positions
and establish at least 80 physical pixels of final displacement. The report
records positions and observed spacing, not missed refreshes, easing quality
or frame-by-frame regularity. A missing/rejected move is a failed scenario.

The only capture routes are Vela's calibrated ext-image-copy-capture path and
Plasma's calibrated KWin ScreenShot2 path. PNGs, request/receipt timestamps,
pixel fingerprints, marker positions, injection intervals and action results
are archived and re-audited offline. The report links before/after PNGs. No
portal, permission dialog or automatic backend substitution is used.

CPU tests cover action detection, missing/static responses, wrong window IDs,
geometry/route/hash rejection, schedules, receipts, export and raw-data audit.
The isolated Vela test captures actual standard window movement while the
button is held, with the cursor included: the grip must keep it away from the
encoded identity. The isolated KWin test identifies the scene through ScreenShot2.
These tests do not qualify physical kernel input or native shell actions.
The first real-TTY campaign, `36c9f47c6ba841ff844ec5ddca803db8`, passed both
responsiveness blocks. Its Vela motion block was rejected when the cursor
covered one marker bit; the original failed result is retained. The corrected
grip passed both real-TTY motion blocks in fast campaign
`dc4fe800fca6426db3c08caabc698afb` on 2026-10-22: 12 captures, ten identified
positions and 162 physical pixels of displacement per desktop. All 123 exported
evidence-file hashes, raw metrics, controls, receipts and restoration checks were
verified; there were no cleanup errors or greeter recovery restarts.
These separate campaigns establish initial-machine fast acceptance of the
implemented P3 actions. Their scene builds differ, and they provide no repeated
performance-comparison evidence; normal P3 repetition acceptance remains unrun.
Failed analyses keep their captures and input intervals alongside the error.
Alt+Tab/context menus, shell-panel and
desktop-switch animations, observations under slow-producer load and
fine-grained motion remain explicit coverage gaps; full-suite and automated
complete-family comparison flags stay false.

### Integration with current upstream tools

The 2026-11-01 integration includes upstream `efdc9c6`. The shared
`vela-input` retains the interactive/checkpoint modes and the benchmark's
fixed evdev-keycode action. The shared `vela-slowgpu` retains portable linear
DMA-BUF allocation, cooperative dispatches, developer `--monolithic` and
benchmark JSON/deadline controls. JSON calibration now records `dispatches`:
the fixed 150 ms campaign request uses 50 dispatches, while GPU timestamps
still span the whole job. This changes the stimulus from historical
single-dispatch runs; equal requested delay is not comparison compatibility.
Changed instrument hashes require a new campaign, not resume of an old one.

The new recorder/capture probe are available for development but are not
installed benchmark instruments or replacements for the declared capture
routes. See the [upstream integration review](../docs/benchmark-suite.md#upstream-integration-review-2026-11-01)
for reuse decisions, timing limitations and the P4 implementation order.

## P4 scaling pattern and session resources

The fixed `p4-pattern-resources-v1` workloads now run through `run` with
independent `scaling` and `overhead` selections. They use the same isolated
sessions, immutable block receipts, three repetitions/default or one/`--fast`,
checkpoint/resume, hardware restoration and raw-data re-audit. Physical TTY
acceptance of P4 is pending; this remains partial family coverage.

Build and refresh the installed instruments from the same checkout:

```sh
cmake --build build-native-displays --target vela-pattern vela-bench-input vela-bench-probe vela-bench-desktop vela-screen-dump
sudo scripts/vela-bench setup --tools-dir build-native-displays/tools
scripts/vela-bench plan --phase scaling --phase overhead --target vela --target plasma --fast
```

From a real TTY after signing out of the personal desktop:

```sh
sudo scripts/vela-bench run --phase scaling --phase overhead --target vela --target plasma --fast
```

Omit either phase to run it alone. Omit `--fast` for three complete blocks per
condition; it never reduces the scale/state/sample/work matrix. Changed source
or binary hashes need a new campaign, not a resume of older P2/P3 results.
The installed desktop is measured; building these tools does not install Vela.

`scaling` runs the native Wayland coordinate pattern at 100/125/150/175/200%,
each opened, snapped left/right, maximized and restored: 25 captures per
desktop/repetition. It uses the desktop's default shortcuts, validates native
configure/preferred-scale/buffer/viewport evidence, applies and reads back each
output scale, and retains the original physical-output PNG. The pointer is
parked at the output's bottom-right corner before rendering, with its injection
receipt retained to prevent cursor contamination. A dependency-free bounded
PNG decoder rechecks pixels offline. The fixed eight-logical-pixel
corner exclusion plus pixel-grid allowance is recorded; a maximum two-pixel
edge overhang is located and counted as offscreen pixels, not silently cropped
away. Expected buffer size is established by the client, not inferred by
shrinking the rectangle to whichever pixels matched. Different pixels and
offscreen pixels are measured outcomes: a completed instrument run can report
nonzero errors. Wrong scale/backend/identity/state, truncated PNGs or failed
restoration invalidate the run. Scale 1 is restored before normal teardown,
including after a failed case; PID 1 retains the overall recovery responsibility.

`overhead` does not run input, presentation or capture calibration. It waits
ten seconds after session readiness, then records a 30-second idle window,
1,800 native client animation submissions and a fixed local SHA-256 workload
(16,384 hashes of the same 1 MiB corpus). The latter is synthetic work, not a
browser/application benchmark. Completion evidence is checked against sampling
intervals. Sampling is once per second without catch-up bursts. CPU comes
from the entire reserved UID's cgroup, including exited processes, expressed
as seconds and percent of one logical CPU. Per-process PSS includes services
outside the compositor child tree; process identity is PID plus start time.
Workload/service membership is retained separately, as are vanished processes
and non-atomic inventory intervals. Root-controller CPU is reported without
subtracting estimated collector costs; user-unit helpers remain in UID totals.
Missing PSS/cgroup access fails explicitly. GPU/energy are `unsupported`, not
zero; shared system services are not attributed to the desktop.

The HTML links original scaling captures and reports independent-block CPU/PSS
and pixel-error statistics without pooling repetitions. Exports recompute
pixel and resource metrics, workload completion and session UID binding.
Coverage stays `partial-pattern-only` / `partial-session-resources` and
`full_suite_complete=false`. The Qt/GTK/browser/Xwayland corpus, resource
interaction sequence, optional GPU/energy sources and complete-family
comparison remain open.

### P1 scheduled block exercise

The common executor can run the planned phase/target/repetition slots before
the corresponding measurement scenarios are implemented. `--exercise` puts
**the same fixed instrument-control workload** in each selected phase slot:
fresh profile/session, presentation/input/capture calibration, verified
conditions and full teardown/restoration. Phase names identify schedule slots;
this mode does not run responsiveness, motion or other family measurements.
It does not inject the qualification suite's crash/hang faults.

Preview without leaving the desktop:

```sh
scripts/vela-bench plan --exercise --phase motion --phase overhead --target vela --target plasma --fast
```

Refresh the installed Python launcher with `sudo scripts/vela-bench setup --update`
(the existing native tools are retained). Log out of the personal
desktop, log into a real local TTY and start the short multi-phase exercise:

```sh
sudo scripts/vela-bench run --exercise --phase motion --phase overhead --target vela --target plasma --fast
```

One START authorizes all four blocks. Omitting `--fast` performs three complete
repetitions per phase and target, without changing per-block controls or
sample counts. For a focused normal-mode acceptance run:

```sh
sudo scripts/vela-bench run --exercise --phase motion --target vela --target plasma
```

That schedules six fresh sessions. Omitting `--phase` selects all five phase
slots (30 blocks for two targets in normal mode). The printed estimate is
provisional; each block retains the 600 s worker and 300 s recovery deadlines.
Phase changes, block IDs, target and repetition are recorded in the plan and
journal. The root launcher rebuilds this fixed schedule, validates the
worker's block binding and requires its instrument-control receipt before
publishing a durable completion marker. No custom command interface is added.

Resume with the **same arguments**, adding `--resume CAMPAIGN_ID`. Completed
blocks are hash-verified and retained; incomplete blocks get a new attempt,
profile, session and calibration. Changed phase selection, mode, seed,
installation or environment is refused. Ctrl+C during a block stops the
supervised worker and journals interruption; export that interrupted campaign
with `sudo scripts/vela-bench campaign-report` if needed before resuming.

Exercise exports record `execution_mode=exercise`,
`measurement_blocks_executed=0`, P0 qualification `not-requested` and explicit
`exercised-not-measured` family coverage. This path passes CPU coordinator/
worker contract tests and passed real-TTY P1 campaign-core acceptance on the
initial Vela/Plasma machine on 2026-10-22:

- `42889274d6e149358f189b23b884dd80`: motion/overhead with `--fast`, four blocks.
- `fb409488498f48be8c3939df8af154e0`: motion in normal mode, six blocks.

All 443 exported file hashes and all ten raw audits, block receipts,
completion-marker hashes, phase journals and hardware restorations were
independently verified; there were no cleanup errors. Both runs used the
same installation and resolved hardware conditions, with identical per-block
controls. Their successful first attempts establish live scheduling and
repetition behavior; interrupted-block checkpoint/resume remains covered by
CPU tests rather than a new real-TTY resume exercise. The P0 archive above
separately establishes its physical crash/hang recovery evidence. These passes
do not establish family measurements or support for additional platforms.

An offline `report.html`, canonical `summary.json` and raw JSON/JSONL/PNG
files, the campaign journal and attempt/completion records are automatically
exported under `benchmark-results/campaign-ID/`,
owned by the invoking operator. Original root evidence is unchanged. Recover
an export with `sudo scripts/vela-bench campaign-report` (use a fresh
`--output PATH` if that export already exists). Validate or regenerate it
without root using `status PATH` and `report PATH`. Reports distinguish
selection completion, P0 instrument qualification and the incomplete full
five-family suite. A pass is scoped to the selected machine, versions and
capabilities; mocks/headless traffic never produce hardware qualification.

To remove a completed setup:

```sh
sudo scripts/vela-bench uninstall
```

Uninstall requires a stopped/restored pilot, verifies ownership and removes
the account/group, disposable state and installed launcher/PAM/unit files.
It keeps pilot evidence and the ownership record under `/var/lib/vela-benchmark`,
other benchmark result archives and normal system logs. It never promises
zero traces. A later setup can reuse that explicitly uninstalled evidence
directory while provisioning a new account and installation identity.

The mechanisms follow the systemd documentation for
[PAM and filesystem namespaces](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html),
[per-user managers/runtime](https://www.freedesktop.org/software/systemd/man/latest/pam_systemd.html)
and [display-manager seat integration](https://github.com/systemd/systemd/blob/main/docs/WRITING_DISPLAY_MANAGERS.md).
These references do not replace real-TTY validation of this implementation.

## Archives

`manifest.json`, `plan.json`, `check.json` and `events.jsonl` are canonical
preflight evidence. The immutable plan includes environment and suite-source
fingerprints. `blocks/check.json` is published atomically after evidence is
closed and synced, and records its hashes. A missing completion record means
interrupted work. Truncated journals, changed evidence and inconsistent
identities are rejected when opening an archive. These checks detect accidental
changes; they are not signatures or authenticity guarantees.

`summary.json` and `report.html` are derived offline. HTML has inline styles,
escaped observed text and no scripts, network assets or server. Preflight
success is separate from full-suite completion: every preflight report states
that it contains no performance measurements. Calibration and exercise campaigns use their separate checkpoint/export
schemas. P2 uses the execution-campaign schema for client scenarios and independent
block statistics; complete-family comparisons remain pending.

## Presentation diagnostic

Build the optional helper with `VELA_BUILD_TOOLS=ON`:

```sh
cmake --build build --target vela-bench-probe
build/tools/vela-bench-probe --frames 120 --timeout 10 > probe.jsonl
build/tools/vela-bench-probe --frames 12 --delay-ms 150 --timeout 10 > delayed.jsonl
```

These commands open an animated window in the selected Wayland session.
The client uses standard xdg-shell, SHM and `presentation-time` v1, records
the advertised presentation clock, each submission, presented/discarded
feedback, refresh/sequence/flags, receipt times and completion counts.
Buffers are reused only after release. Missing protocols, incomplete
feedback and timeouts produce a nonzero exit status; a stalled startup is
bounded too. A deadline can leave a partial JSONL stream without a summary.

`--delay-ms` inserts a known **client CPU sleep** and records its actual
duration. It is an instrument control, not the B3 slow-GPU workload.
Submission-to-presentation evidence is not input-to-presentation or optical
latency. Frame callbacks pace submissions; they are not used as presentation
timestamps. This diagnostic does not qualify capture, native shell motion,
physical-display timing or any of the five benchmark families. Headless
tests verify protocol handling only.

## Tests

The coordinator tests use temporary directories and mocked host inventory:

```sh
PYTHONPATH=benchmarks python3 -m unittest discover -s benchmarks/tests -v
ctest --test-dir build -R '^(benchmark-core|benchmark-probe-cli)$' --output-on-failure
ctest --test-dir build -R '^functional-benchmark-probe$' --output-on-failure
ctest --test-dir build -R '^benchmark-plasma-integration$' --output-on-failure
```

The CTest core entry is registered on Python 3.11+. The native CLI suite needs
the compiled helper but no GPU; it includes a connected, silent socket to
test startup deadlines without a desktop. The functional suite starts an
isolated headless Vela with a real Vulkan GPU and checks feedback identity,
clock ordering, a known 150 ms client delay, raw F24 input mapping through
the isolated test transport, output apply/read-back and captured frame IDs.
The test transport does not qualify kernel uinput injection. An isolated
KWin virtual session also passed native output query/apply and ScreenShot2
capture/marker decoding during development. GPU unavailability is a skip,
not a pass. None of these tests switches the operator's desktop session.

The optional Plasma integration entry starts a real KWin virtual compositor
on a private bus with a disposable HOME/runtime and a custom socket name.
It checks missing `WAYLAND_DISPLAY`, peer identity, rejection of foreign and
ambiguous listeners, Python preflight and native KScreen output queries.
Only NSS and the user-manager query/privilege-drop boundary are fixtures;
KWin, DBus and socket operations are real. It proves neither the physical
greeter's logind/PAM state nor hardware takeover/restoration. To keep its
JSON evidence locally, run unprivileged:

```sh
python3 benchmarks/tests/plasma_integration.py build-native-displays/tools/vela-bench-desktop benchmark-results/plasma-preflight-selftest/real-kwin.json
```

Account/session tests simulate NSS, system commands and process inventories;
they exercise real profile files only inside temporary test directories.
They cover ownership/TTY guards, fresh profiles, state transitions, concurrent
operation rejection, restoration failures, reboot handling, uninstall, hardware read-back and
monitor replacement, input/capture rejection, sensitivity/A-A acceptance,
fault signal ownership and campaign checkpoint/resume/export integrity.
Optional `systemd-analyze verify` checks the generated unit syntax. If missing
or denied socket options by a sandbox, that check is explicitly skipped and
must be rerun outside that restriction. A passing static check does not open
PAM sessions or qualify a real seat.

An explicit user-manager integration test reproduces start limiting across
clean stops with harmless `true` units and verifies recovery. A separate
failed `false` unit verifies that an existing failure is not reset. It uses
fresh transient unit names and retains a D-Bus reference across stops; every
unit is stopped and released afterwards. Only the unit name and system/user
manager boundary are substituted. It does not touch the display manager,
PAM, TTYs or hardware. The same test freezes a TERM-ignoring temporary process,
forces its stop deadline, reproduces the occupied session-name error and
verifies that release permits a new service with that same name. It requires
PyGObject and a running user manager:

```sh
python3 benchmarks/tests/manager_integration.py benchmark-results/manager-recovery-selftest/evidence.json
```

The alias reservation regression uses private randomized user templates running
only `/usr/bin/true`. It reproduces misleading canonical mask state through an
alias, applies the production reservation logic and verifies both names reject
activation. It cleans up its own units afterwards and requires PyGObject and a
running user manager. Run unprivileged; it does not address physical TTYs,
gettys, desktop sessions or system services:

```sh
python3 benchmarks/tests/vt_integration.py
```
