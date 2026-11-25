# Benchmark runtime optimization

Draft design for [#228](https://github.com/vela-desktop/vela/pull/228), stacked on
[#109](https://github.com/vela-desktop/vela/pull/109) at `eec2044c` (2026-11-25).
The commit identifies the runner reviewed for this design. A measured baseline
still needs to be collected on physical hardware.

## Objective

Reduce the elapsed time from accepting `run-all` to receiving both audited
reports, with benchmark reliability at least as high as today. Measure time
spent with the host under benchmark control as well. The first candidates should
remove work performed by the runner outside the measurement windows, where the
risk of changing the Vela/Plasma comparison is lowest.

Measure observer overhead as a second outcome. CPU, I/O, D-Bus traffic and
compositor work caused by probes or capture can alter what Vela and Plasma do
during a measurement. An instrument change that lowers this load can improve the
benchmark even if a required pacing interval absorbs the elapsed-time gain.
Report that improvement separately from campaign speed.

## Current benchmark

[`run-all`](../benchmarks/README.md) runs two campaigns after one START. It
refreshes the launcher, handles pending recovery, preflights both selections,
then runs 30 P2/P4 blocks and 18 P3 blocks for Vela and Plasma in normal mode.
`--fast` runs one complete repetition (10 P2/P4 blocks and six P3 blocks).
Each campaign produces its own archive and report. The normal and Vela sync-test
profiles are distinct experimental conditions and bind into the plan.

| Workload | Contract to preserve |
| --- | --- |
| P2 `p2-neutral-client-v3` | 105 input actions (100 measured) and 630 animation submissions (600 measured) per applicable condition, with eight control/loaded conditions in slow-apps. Preserve sequential feedback-gated input, delayed-producer and contention coverage, capture-off windows, presentation/discard accounting and UID/device provenance. |
| P3 `p3-desktop-observation-v3` | 12 original full-output captures per action sequence, including 24 paired control/loaded sequences in slow-apps. Preserve the capture-to-input dependencies, sample timing, source receipts and pixel audit. P3 measures coarse visible state and response bounds. |
| P4 `p4-pattern-resources-v6` | 25 native scaling cases plus up to 100 toolkit cases with the same ordered matrix, exact identities and explicit unsupported outcomes. Preserve output and crop evidence. For resources, preserve capture-off windows, the 30-second idle interval, 1,800 animation submissions, 12 maximize/restore cycles, fixed SHA-256 work and the declared CPU/PSS scope. |

Every block retains a fresh real-TTY session and disposable profile, the frozen
autostart and credential policy, the planned desktop/instrument identities,
physical-input isolation, and PID 1-supervised cleanup. Preserve the controls
that detect active VT or session loss, device changes, monitor failure and
workload faults. A failed block may yield to the next independent block only
after verified restoration and context checks. Preserve the nonzero result for
`completed-with-failures`, cancellation, and resume of unsuccessful blocks only.
Keep the current DDC retry and read-back semantics, greeter recovery, brightness,
output and power restoration, and worker/input/UID release receipts.

The parent now calibrates delayed GPU work against Vulkan timestamps, with
bounded stable convergence and fixed iterations during measurement. Older
physical acceptance and the newer focused P2 checks cover their own versions
and selections. They cannot supply a current whole-suite baseline. See the
[parent PR](https://github.com/vela-desktop/vela/pull/109) for validation scope.

## Collect a baseline

Start with coarse monotonic spans around existing boundaries. Reuse instrument
receipts and the [event journal](../benchmarks/vela_bench/archive.py).
Record which campaign, block and attempt produced each span, with boot ID,
clock and outcome. Keep interrupted spans visible. Avoid per-action disk writes,
extra capture and high-frequency polling for instrumentation. Add a targeted
span only when the initial breakdown leaves a material cost unexplained.

- Time launcher refresh, pending recovery and preflight after START. During a
  block, distinguish takeover/session readiness, condition changes and controls,
  measured work, and teardown/restoration. Track failed or resumed attempts and
  the time to restore and reacquire after a failed block.
- Time acceptance audit, checkpoint and campaign inspection, export copying and
  verification, and report rendering. `run-all` exports P2/P4 before entering P3.
  Record the final verified return of the host and the time both reports finish.
- Attribute time on the critical path using enclosing elapsed spans and the
  union of overlapping work. Record runner CPU, bytes read/written and subprocess
  counts where they can be captured cheaply, with the accounting boundary. Reuse
  existing CPU and capture-control receipts for interference checks.
- For changes near a measured window, retain actual P2 input intervals from the
  injection/feedback receipts and P3 request/receipt intervals and dependencies
  from the capture/action receipts. Check per desktop and workload whether a
  faster instrument would advance the stimulus or merely leave more slack.
- Reconcile the spans against end-to-end elapsed time. Use boot-local durations
  for resumed attempts and report elapsed reboot or operator recovery separately
  when observed externally. Measure failed attempts and retries as operating cost
  alongside the elapsed time and acceptance rate of complete campaigns.

The first output is an elapsed-time breakdown by target, phase and outcome, with
the unaccounted portion visible. Planning estimates and watchdog limits remain
useful for scheduling. Use the measured breakdown to choose the first change.

## Candidates in priority order

### 1. Audit and export the same evidence with fewer passes

The [P3 audit](../benchmarks/vela_bench/desktop.py) already caches detector output
for byte-identical extended captures within an audit. For responsiveness and
motion, the legacy base audit decodes its PNGs and the enclosing P3 audit decodes
them through its own reader. Check how much time is spent there and in repeated
hashing. A candidate can share decoded detector output from the same verified
bytes within an audit while keeping both metadata comparisons and all
per-artifact checks. Give offline replay the same raw input and verify it
independently of the live result. Keep historical readers and rejection behavior.

The [campaign code](../benchmarks/vela_bench/campaign.py) hashes evidence at
checkpoint, rechecks completion records during inspection, then verifies the
source and destination around export. Profile these passes separately. Where
several checks legitimately operate on the same bytes in one operation, try
sharing a bounded read or computing the digest as bytes are copied. Preserve
binding to the accepted manifest, destination verification, checks for a
changing source, file ownership and path validation, and durability and resume
semantics. Reject a changed, truncated or substituted file before it can be
reported as accepted. Keep independent revalidation across trust boundaries.

These candidates can be screened on existing archives. Compare derived values,
accepted/rejected outcomes and exported evidence, and exercise corrupt or
malformed PNGs, changed bytes, interrupted export and a fresh offline re-audit.

### 2. Reduce capture overhead where the timing contract allows it

[Vela capture](../benchmarks/vela_bench/calibration.py) starts `vela-screen-dump`
and a separate marker analysis process for each image. Plasma already batches
native ScreenShot2 requests for a sequence. Profile startup, acquisition, PNG
encode/decode, transport and validation separately on both desktops. Start with
work outside sampled intervals. Consider a persistent serialized capture path
only if startup or transport is material and the same backend and timing
semantics can be retained. Preserve per-request identity, output, original PNG,
request/receipt bounds and bounded cleanup. Exercise stale and duplicate
captures, failed or stalled workers and backend or output changes.

P3 injects input in response to particular captures. Preserve the required
before/after ordering and the observed capture-to-action schedule within a
qualified envelope. Compare cadence and observer bias for each target,
including capture-on contention. Retain capture-off coverage for P2 and P4
resources. If a change alters the stimulus or the meaning of a capture
timestamp, qualify it as a new method with an explicit archive version.

### 3. Revisit measured setup and wait costs

Consider a readiness signal for a costly setup wait only where it proves the
same state. For P4 visual checks, require the requested output mode and the
correct client state in the captured image. A Wayland configure receipt alone
establishes geometry negotiation. Replacing visual settling needs evidence the
expected frame or stable state is actually observable on both desktops and for
each affected toolkit. Preserve the existing wait until that evidence exists.

Per-action health checks in [P2 input](../benchmarks/vela_bench/calibration.py)
run between feedback and the next injection. Keep the existing checks unless a
measured cost justifies a candidate with continuous coverage of each mutable
invariant, loss detection, fail-closed invalidation and unchanged emergency
recovery. Compare actual stimulus intervals and control/loaded coverage before
accepting any timing-sensitive change.

P4 scale-major reordering, cross-block calibration reuse and campaign-wide
host takeover are outside the initial scope. Each changes ordering, validity
scope or recovery architecture. Reconsider one only when the baseline shows a
large enough opportunity and a separate design covers method/version impact and
fault recovery.

## Validate speed and reliability

1. Screen each candidate independently on real archived evidence or complete
   affected blocks. For live changes, measure the profiler's own cost and run
   baseline against baseline to establish repeatability. Match installed
   desktop/instrument versions, host, outputs, refresh, power, profile, corpus
   availability and seed within a comparison. Record runner revisions and
   intentionally changed instrument identities. Control for thermal drift.
2. Choose the timing and non-inferiority margins before comparing candidates,
   using the repeatability data and the smallest measurement effect the workload
   should distinguish. Check each desktop's acceptance, preserved work and raw
   audit, latency/cadence distributions, achieved producer load and capture
   coverage. Keep the known-delay sensitivity check. Use faults such as stale
   capture or device loss to verify that compromised blocks are rejected, and
   check that unsupported and interrupted outcomes keep their proper status.
3. Exercise the failure path of each change and the applicable private Vela/KWin
   integrations. Confirm restoration and next-block behavior after recoverable
   failures, and stop behavior after cancellation or unverifiable restoration.
   Use real-TTY qualification on both desktops for changes to live measurement
   or recovery. Keep archived evidence auditable with the method under which it
   was produced.
4. Confirm an end-to-end speed claim with independently repeated, counterbalanced
   physical `run-all` comparisons across Vela and Plasma. Track distributions,
   uncertainty, failed attempts and recovery costs. Qualify `--fast` and normal
   mode for any claim about both. Include observer overhead and host-occupancy
   time alongside START-to-reports elapsed time. Publish the run IDs and raw
   timing evidence with the result.

Start with coarse timing and use the baseline to choose the first measured
hotspot. Advance changes that demonstrate a wall-clock or observer-overhead
benefit and meet the reliability gates.

## Implemented in this draft

The runner now records boot-scoped timing in the `run-all` receipt, per-attempt
checkpoints and the existing event journals. The [operator guide](../benchmarks/README.md)
describes how to join those records with instrument receipts to estimate host
occupancy. The P3 audit hashes each PNG and derives its pixels from the same
read, reusing only detector results for byte-identical images. Campaign export
checks copied evidence against accepted checkpoint hashes and verifies the
source file stayed stable during the copy. Offline reports still run an
independent raw-evidence audit.

Offline replay showed identical P3 audit results on two archived blocks and
identical export manifests. See [#228](https://github.com/vela-desktop/vela/pull/228)
for the tested selections and timings. Physical campaign timing and acceptance
are pending. Capture, workload pacing and takeover changes still require
measurements and the qualification described above.
