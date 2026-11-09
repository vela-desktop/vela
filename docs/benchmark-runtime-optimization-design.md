# Benchmark runtime optimization — design proposal

**Status:** Draft / design-only; no implementation authorized by this document  
**Parent:** #109 (`feat/benchmark-suite`)  
**Target:** reduce end-to-end wall-clock runtime while preserving meaningful benchmark workloads, evidence, isolation, and recovery  
**Scope:** the installed Vela/Plasma benchmark paths currently implemented in #109; no change to supported desktops  
**Parent integration baseline:** #109 `8ff6b83` (2026-11-08). Refresh this reference and re-check invariants after any subsequent parent update; this is **not** a measured speed baseline.

## 1. Problem and goals

The benchmark runner in #109 prioritizes complete, reproducible evidence and supervised physical-machine recovery. That is the right default, but its elapsed time includes significant work unrelated to the measured desktop behavior: repeated session takeovers, per-action host queries, process creation for capture, repeated PNG decoding, repeated output-scale configuration, and fixed settling intervals.

This proposal treats **time spent measuring the system** and **time spent operating the harness** separately. The objective is to reduce the latter before changing any statistical workload.

### Goals

1. Reduce **total elapsed campaign time**, measured from the accepted START to verified return of the machine, for both `--fast` and the default three-repetition mode.
2. Preserve *the same* per-condition input actions, submitted frames, fixed work, screenshots, independent repetitions, source evidence **and temporal stimulus contract** unless a separately reviewed/versioned methodological change is explicitly justified.
3. Reduce observer/interference overhead in measured windows, particularly repeated external process and D-Bus activity.
4. Preserve the real-TTY isolation model, fresh desktop profiles and sessions, crash/hang/hotplug recovery, original evidence, offline re-audit, and fail-closed behavior.
5. Make actual savings measurable, attributable, and repeatable, including invalid blocks, retries, and machine recovery.
6. Prioritize **net critical-path reduction** over gross work eliminated: an optimization that merely replaces CPU work with mandatory pacing may improve observer interference without shortening the campaign.

### Non-goals

- Making `--fast` use fewer actions or frames: it must continue to mean one *complete* independent repetition.
- Changing 100 measured input actions, 600 measured animation submissions, the eight P2 loaded/control conditions, 12 P3 captures per action, 125 P4 scaling cases (where fully supported), the 30-second resource idle interval, 1,800 active-animation submissions, or 12 window maximize/restore cycles as a shortcut.
- Replacing or weakening raw evidence, hashes, source-identity checks, pixel oracles, sandbox policies, original capture backends, or cleanup requirements.
- Parallel benchmark sessions, simultaneous desktop comparisons, or overlapping workloads that would interfere with resource/latency measurements.
- Claiming a numerical speedup without timing a real accepted campaign.
- Treating unchanged action/frame counts alone as proof of identical timing or observer behavior.

## 2. Baseline and concrete observations

At parent commit `8ff6b83`, the existing code establishes the following **work counts**, not actual elapsed durations:

| Path | Existing work and repeated overhead | Relevant code |
| --- | --- | --- |
| Campaign orchestration | One supervised `begin()` and separate pilot lifecycle per target/phase/repetition block; normal 5 phases × 2 desktops × 3 repetitions = 30 blocks | `benchmarks/vela_bench/campaign.py`, `pilot.py` |
| Session protection | `health()` calls `verify_session()`, which performs logind, systemd-user, D-Bus and namespace checks; P2 input invokes it for each action | `pilot.py`, `calibration.py` |
| P2 slow-apps | Eight conditions; each includes 105 input actions and 630 animation submissions (840 actions/5,040 submissions total) | `measurements.py` |
| P3 slow-apps | 24 action sequences × 12 full-output screenshots = 288 captures per desktop/repetition | `desktop.py` |
| Vela capture | Each capture may start both `vela-screen-dump` and `vela-bench-desktop --analyze` as subprocesses | `calibration.py: capture_worker` |
| P4 scaling | 25 native-pattern cases + 100 four-toolkit corpus cases, with output scales applied separately for native and each toolkit family | `platform_workloads.py`, `corpus.py` |
| P4 fixed waits | Native: 25 × 0.6 s plus 25 action sleeps × 0.7 s = 32.5 s; corpus: 100 × 0.25 s plus 100 action sleeps × 0.35 s = 60 s, assuming all four corpus families supported | `platform_workloads.py`, `corpus.py` |
| PNG verification | Python per-byte PNG unfiltering; original screenshot re-decoded by immediate metrics and offline audits, sometimes multiple times | `pixels.py`, `desktop_pixels.py`, `corpus.py` |
| P0 instrument controls | Two 120-frame presentation diagnostics and input/input-capture/input-delay calibration are repeated per relevant block; overhead already skips these | `pilot.py` |

**Parent compatibility snapshot (`8ff6b83`; inspect again before implementation):**

- **P2 `p2-neutral-client-v3`:** preserve its 105 input actions / 630 animation submissions per condition, eight slow-apps scenarios, paired control/load windows, producer UID/device binding, and the motion window that includes trailing discarded feedback. V3's feedback-window ordering, accepted vblank-relative presentation receipts and historical v1/v2 readers are part of the evidence contract; do not silently reclassify a discarded frame or change the latency definition to win time.
- **P3 `p3-desktop-observation-v3`:** preserve 12 original captures per action, 24 loaded/control slow-apps sequences, capture-enabled contention attribution, stable-state/original-PNG verification, source UID identity and historical v2 audit support. The current offline pixel-audit cache reuses only detector data for byte-identical SHA-256 digests **after verifying each persisted image hash**; that already-implemented optimization must not be claimed again as T3a savings.
- **P4 scaling v6:** preserve 25 native plus up to 100 controlled toolkit cases, all fresh clients, exact corpus/source/font/tool identities, screenshot/crop oracle, frozen output conditions, the strict unsupported-case protocol and old archive readers. Reordering the matrix requires its own explicit new ordered spec, not a reinterpretation of v6 evidence.
- **Campaign and recovery:** recovered failed blocks now remain as failures and **independent subsequent blocks may continue only after verified host restoration**, confirmed worker/input release, reserved-UID teardown, unchanged conditions and same boot. Cancellation, incomplete recovery, integrity failure or changed conditions still stop; a completed schedule with failed blocks is `completed-with-failures` and exits nonzero. Existing restore/continue receipts and unsuccessful-block-only resume must survive any runtime refactor.
- **Safety and reference profiles:** private-session takeover now freezes and validates the system autostart inventory; per-user XDG overrides suppress accessory autostarts while retaining core services. Profile policy, inventory and binary/file identities bind into immutable plans. Display-manager authentication is quiesced before teardown; `InputIsolation` monitor failure immediately requests PID 1-supervised recovery, and release is verified after worker exit. These controls and their ordering cannot be amortized away without equivalently demonstrated coverage.

**Important:** The sum of fixed sleeps is only a lower bound for those explicit waits, not an estimated achievable saving. `model.BUDGETS` and campaign's printed duration ranges are planning allowances, not measured per-stage costs; systemd deadlines are upper bounds, not automatic delays. P3 `observe` is a separate selection, not part of the ordinary five-phase `run` matrix.


## 3. First requirement: profile the harness before optimizing

Implement low-intrusion, monotonic, structured **harness stage timing** *before* any optimization. Each event must include campaign/block/attempt IDs, target, phase/repetition, method/spec, stage, start/end `CLOCK_MONOTONIC` timestamps, status and, where relevant, subprocess exit and tool identity.

Also capture the **boot ID** (e.g. `/proc/sys/kernel/random/boot_id`) and clock domain. Monotonic timestamps are comparable only **within the same boot and clock domain**: a resumed campaign after reboot must not concatenate them or calculate a fictitious continuous duration. Report attempt-local durations and externally observed recovery elapsed time separately.

Profile preflight/context refresh, frozen autostart/profile inventory, display-manager authentication quiescence, VT/greeter setup, display-manager transitions, reset/profile, session launch/readiness, condition application, presentation probes, input/capture calibration, fixture launch, settling, measured work, capture backend and transport, image analysis, hashes/audit, checkpoint/fsync, input release / reserved-UID teardown, continuation checks and recovery. Preserve partial/failed stage timing; incomplete evidence must never be accepted. A critical-path breakdown must account for overlap rather than naively summing concurrent durations. Track process-spawn counts and harness CPU time when available.

**Freeze a temporal baseline:** in addition to elapsed stages, record actual input injection receipts and inter-arrival intervals (including `health()` time), capture request/receipt/analysis timing, P3 capture-to-action dependencies, and state-to-observation windows. Record **feedback completion → check → pacing wait → injection → next feedback completion** separately for P2. Reuse already available monotonic instrument receipts; do not add heavyweight tracing/polling within measured windows merely to profile them.

Compare candidates against a frozen pre-optimization baseline with the same installed desktops, hardware and conditions. Repeat independent whole campaigns, rotate ordering and retain raw stage events. Planner budgets and service deadlines are **not** measured durations. See Section 5 for the preregistered comparison plan.

### Attribution: gross overhead is not net runtime savings

Stage 0 must generate a **time-accounting ledger** by target, phase, block **and attempt outcome** (accepted, failed with verified restoration and continuation, failed without safe continuation, cancelled, integrity-invalid or interrupted/resumed). Preserve the `completed-with-failures`/nonzero distinction, record the extra restoration verification and reacquisition after a recovered failure, and never silently drop an unsuccessful attempt from the denominator. Use these distinct quantities:

| Quantity | Definition | Interpretation |
| --- | --- | --- |
| Gross harness overhead removed | Reduced CPU/process/decode/check/transport time in comparable work | May reduce observer interference without shortening elapsed time |
| Pacing absorption | Time saved by faster work but deliberately reintroduced as waiting to preserve a frozen temporal stimulus | **Not** an end-to-end runtime saving |
| Critical-path savings | Difference in the elapsed campaign critical path caused by the optimization, after overlap and required pacing | Candidate contribution to actual faster completion |
| Net accepted-campaign saving | Change in START-to-verified-restoration wall time, counting invalid attempts, retries and recoveries | **Primary optimization outcome** |

In particular, if a P2 health check becomes 20 ms faster but an equivalent stimulus requires 20 ms more controlled waiting, it produces approximately **zero critical-path savings** for that action. Likewise, reducing capture transport latency may lower CPU overhead yet save no wall time when the next action has a fixed deadline. Neither is grounds to claim a faster benchmark.

Attribute candidate savings by category: (a) overhead outside the frozen temporal workload, (b) overhead inside a window but off the critical path, (c) overhead inside a window absorbed by pacing, and (d) savings requiring a **new method/stimulus version**. The latter must be evaluated as a different benchmark, not conflated with a like-for-like suite-speedup claim.

Use measured critical-path weights, not equal weighting of phases. A 20% improvement in a stage accounting for 1% of campaign time cannot by itself justify a claim of material end-to-end speedup. Where parallel work exists, compute the critical path from the event DAG rather than subtracting independent stage totals.

### Exit criteria for Stage 0

- A reproducible, boot-scoped timing ledger with accepted/invalid/cancelled/recovered attempts, continuation/restoration receipts, and an explicit gross-versus-net attribution; the ledger must not treat a `completed-with-failures` schedule as an accepted completed campaign.
- Frozen P2/P3 stimulus contracts with instrumented receipts and defensible tolerances; do not select a favorable tolerance after seeing optimized results.
- A ranked shortlist of critical-path costs and a rough attainable saving bound for each track.
- A low-overhead profiling validation (including an A/A comparison where practical) showing that the profiler does not materially perturb the workload.
- Before changing a safety-sensitive monitor, a per-invariant coverage matrix and fault-injection plan (see T1a).

## 4. Proposed optimization tracks

### Cross-cutting invariant — temporal workload equivalence

Identical counts do **not** necessarily mean identical compositor workloads. For each proposed optimization document and test three independent guarantees:

1. **Work equivalence:** same types/counts of actions, submitted frames, fixed work, cases, captures, repetitions and safety controls.
2. **Temporal equivalence:** predeclared input intervals, capture-to-action dependencies, minimum pacing, jitter/late-deadline policy, transition timing and observation windows. Currently `calibration.run_input()` performs `health()` **before** its 80–99 ms sleep: a cheaper check changes injection-to-injection timing. In `desktop.Controller.capture()`, P3 actions are issued after selected screenshot receipts: faster capture can accelerate mouse dragging and window actions. Count-preserving optimizations can therefore change the stimulus.
3. **Observation equivalence:** same backend/source/output/UID/frame semantics, original evidence and pixel oracle, capture position in the timeline and observer interference.

Stage 0 must establish the actual baseline and acceptable timing envelope **before** candidate data is inspected. Where applicable, pace input using explicit monotonic deadlines, record real injection receipts, and forbid catch-up bursts after a delayed step. Preserve prerequisites (e.g. P3 pre-action frame) even when scheduling is made explicit. Missing deadlines or gaps invalidate evidence rather than weaken the contract.

**P2 is feedback-gated, not an open-loop fixed-frequency generator.** The current order is: previous action's presented-frame receipt (or initial readiness) → synchronous `health()` → fixed 80–99 ms sleep → injection receipt → wait for the corresponding presented frame → next iteration. The measured next-input interval therefore includes both checking cost **and variable prior-frame completion time**. Any replacement pacer must satisfy **all** applicable gates: previous frame accepted, isolation/health valid, baseline-frozen minimum input spacing, and the declared schedule/overrun policy. It must not inject just because a wall-clock deadline passed, skip a pending presentation receipt, compensate for delayed frames with bursts, or silently alter the relationship between presentation and the next input. Preserve or explicitly version *both* injection-to-injection and presentation-to-next-injection timing.

**P3 is similarly feedback-gated:** capture `i` is validated before a callback may inject commands for the next observation; pointer dragging spans specific capture indices. A faster worker must preserve capture/action dependencies **and** the preregistered drag/window-action time envelope. If the old envelope cannot be defined and checked with adequate confidence, retain the current cadence or publish a new method version rather than describe the result as a harness-only optimization.

If equivalent pacing cannot be proven, **version the method/stimulus**, qualify it independently and do not claim a like-for-like harness-only speedup. Existing P3 coarse observation is not an optical or frame-level latency measurement.


### T1 — Split inexpensive liveness checking from deep session attestation

**Current:** P2 `run_input()` calls `pilot.health()` for each action; it executes input isolation checks, `verify_session()` and `fgconsole`, with external logind/systemd/D-Bus queries. Slow-apps alone involves at least 840 action-level checks per block. Since `health()` precedes the fixed pacing sleep, its cost is part of the current stimulus interval.

**T1a — optimize existing checks first, without another supervisor:**

- Profile individual checks. Classify properties as immutable for the block, cheap mutable state, or those requiring deep attestation. Cache only frozen IDs/expected values, never an unbounded mutable `healthy=true`.
- Prefer persistent D-Bus subscriptions/connections, targeted kernel state reads, `pidfd` liveness where relevant, or equivalent cheap existing-path checks over repeated utility launches; integrate with existing `pilot.py` / systemd and `InputIsolation`.
- Retain deep attestation before/after windows, at key transitions and before acceptance, plus independently proven mid-window change detection. Endpoint checks alone cannot prove an uninterrupted healthy interval.
- Introduce monotonic-deadline input pacing **subject to previous-frame completion and current health validity**, or an equivalent audited feedback-gated timing contract, so cheaper `health()` cannot silently accelerate P2 actions. Record per-action previous-presentation-to-injection and injection-to-injection intervals, overruns and invalidation; **no catch-up bursts**. Treat time spent waiting to preserve the original stimulus as pacing absorption, not a speedup.
- Continue fault-coverage tests for VT/session loss, device hotplug, process boundary violations and emergency abort. Preserve the existing `InputIsolation(on_failure=...)` evidence callback and immediate SIGUSR2/PID 1-supervised recovery trigger: a slower optional checker may never become the only path to emergency recovery. Quantify savings and observer impact.
- Treat the current profile inventory, permitted core-session autostarts, authenticated DM quiescence, executable/file identity and reserved-UID boundaries as distinct verified invariants, not merely cached `healthy` session metadata. Snapshot hash equality before and after a window alone cannot prove uninterrupted protection.

**T1a coverage feasibility gate — required before replacing any deep check:**

| Safety invariant | Evidence T1a must supply | Unacceptable shortcut / fallback |
| --- | --- | --- |
| Active VT and logind session | Authoritative change observation **with loss/overflow detection**, session identity and a bounded revalidation path | A periodic active-VT snapshot alone cannot prove no transient VT change; preserve current checks or escalate |
| User-manager/process lifetime | PID/cgroup identity, lifecycle transitions, missed-event handling and fail-closed response | Caching a previous `systemctl` or `loginctl` success |
| Reserved UID, allowed process tree and namespace | Complete enough event coverage to detect transient unauthorized membership, plus identity-bound evidence | Endpoint-only process scans, unbounded cached process lists, or assuming D-Bus reports every process change |
| Input identity, exclusive grab and hotplug | Current `InputIsolation` guarantees plus authoritative device-change/invalid-grab detection and a bounded invalidation latency | Replacing grab/device checks with an unrelated liveness heartbeat |
| Recovery authority | Independent PID 1 supervisor/watchdog, existing input-monitor failure trigger, unambiguous cleanup/release receipts and bounded recovery | Depending on the workload or the optional monitor to recover itself |
| Profile/autostart and candidate identity | Frozen policy + inventory + relevant file/binary hashes, checks at their current trust boundaries and protection against unauthorized mutation | Reusing previously validated profiles or silently ignoring a changed autostart/executable |
| Display-manager quiescence | Authentication freeze, session recheck and teardown ordering, plus verified unwind on any fault | Stopping the display manager without ruling out a concurrent login |

For **each mutable property** record the authoritative event/source, how event loss is detected, maximum detection latency, revalidation strategy, injected fault, expected invalidation evidence and responsible recovery authority. A polling interval alone is not proof of uninterrupted coverage. If a property cannot be covered in T1a, **retain its existing deep check** or defer that part to T1b; never claim T1a has replaced a stronger guarantee on the basis of lower overhead.

**T1b — privileged event monitor only if T1a is insufficient:**

- Keep it block-scoped and subordinate to the **existing PID 1/pilot recovery authority**, not a competing supervisor. Specify startup readiness handshake, UID/process identity, sequence numbers, heartbeat/freshness deadline, explicit invalidated state and durable event log.
- Add a per-invariant coverage table: logind/user manager, active VT, allowed processes/UID/cgroup, input devices/grab/hotplug and namespace. For each give event source, overflow/loss detection, fallback revalidation, **maximum detection latency**, evidence and invalidation rule.
- On queue overflow, lost subscription, unobserved interval, stale heartbeat, monitor death/freeze or unverifiable identity, reject the block and invoke supervised recovery, even if a later deep check appears healthy.
- Keep evdev emergency abort/grab safety independent. Characterize monitor CPU/process cost and avoid contaminating or asymmetrically affecting P4 resource accounting.

**Acceptance:** inject session/user-manager loss, VT switch, transient unauthorized process membership, hotplug, event loss/overflow, stale state and (T1b) monitor death/freeze; prove bounded detection/recovery and rejection across gaps. Preserve **both** P2 injection spacing and presentation-to-next-injection envelopes and all existing action/frame requirements. A T1a proposal that cannot prove its coverage matrix is **not** qualified, even if it substantially reduces process spawns. Implement T1b only when T1a cannot meet these guarantees or leaves measured overhead worth addressing.


### T2 — Use persistent, serialized screenshot transport in a block

**Current:** Vela P3 captures may invoke `vela-screen-dump` and `vela-bench-desktop --analyze` repeatedly, while P4 uses one-shot capture calls. In P3, `Controller.capture()` consumes 12 rows and injects certain mouse/window actions only after receiving particular screenshots. Faster capture therefore **changes the temporal stimulus** unless explicitly controlled.

**Design:**

- Evaluate a block/session-scoped persistent, **serialized** capture worker retaining the exact Vela `ext-image-copy-capture-v1` and Plasma KWin Screenshot2 semantics. Never substitute portal/software capture or reuse a worker across fresh desktop sessions.
- For every request bind a unique sequence/request ID, associated action, backend, output geometry, UID, source/frame identity, original full-output PNG bytes/hash, and a verifiable response. Stale/duplicated/out-of-order frames, response ambiguity, buffer reuse defects, worker crashes or missing replies fail closed.
- Distinguish monotonic timestamps for client request, backend dispatch, frame completion/receipt, PNG encoding, analysis and consumer delivery. **Do not compare process-start-inclusive legacy request durations directly with new in-process/IPC durations.** Retain provenance for equivalent time segments.
- Preserve **P3 v3** capture-action dependencies, original capture count/order, capture-enabled CPU/GPU-contention receipts, strict source/UID and original-PNG validation, and an explicit **feedback-gated input pacing envelope** based on Stage 0; do not allow an earlier frame delivery to accelerate the drag trajectory. Use monotonic deadlines **only alongside successful prior capture receipt and validation**, log capture-to-action and action-to-next-capture intervals, and invalidate missed/ambiguous steps rather than emit bursts. Attribute any time made idle by pacing to absorption, not net wall-clock saving.
- Keep captures synchronous whenever the next action depends on the captured image. Defer analysis only where the next stimulus and measured windows cannot be affected. No capture during P2 neutral or P4 resource conditions where prohibited.

**Acceptance:** replay of original PNG and source identities, exact sample count/order, timestamp semantics, backend/output/UID/frame checks, worker failure/reconnect cases, P3 pointer/window action pacing equivalence, and separated before/after backend-capture versus IPC/encoding/analysis costs. If stimulus timing changes, make it a **new method version**. Do not interpret reduced capture overhead as a measured desktop responsiveness improvement.


### T3 — Remove redundant PNG work first; native decoding only if warranted

**Current:** `pixels.png_rgb()` unfilters PNGs in Python. In P4 corpus, `image_metrics()` decodes the original, then `row_metrics()` calls `image_metrics()` again and separately decodes/validates the crop. P3 offline audit **already** caches derived detector data for identical image SHA-256 values inside one audit, while still recomputing each persisted PNG hash. The parent also added a bounded `image_bytes()`/`image_hash()` path: do not claim these existing protections or per-audit cache as new optimization work.

**T3a — zero-new-dependency first step:**

- Pass already verified unresampled RGB and derived metrics between consumers in the *same trust stage*, rather than decode and recalculate repeatedly. Keep caches bounded and tied to content hash and evidence/source identity.
- Always independently hash every persisted source file, check the exact crop pixels, verify backend/output/UID/source receipts and perform at least one **independent offline raw audit**. Do not let a same-stage cache substitute for later on-disk verification.
- Remove process creation only where profiling proves it is extraneous and the temporal workload/observer contract remains equivalent. Record decode count, CPU cost, wall time and spawn reduction.

**T3b — optional native decoder after T3a is measured:**

- Evaluate a bounded native unfilter/decoder or pinned library plus strict validation wrapper, preserving signature, chunk/CRC/ordering, decompressed length, dimensions, accepted bit depth/color type, non-interlace and all existing rejection limits.
- Require byte-for-byte identical RGB, original/crop hashes, derived metrics and rejection behavior on malformed/truncated/oversized/fuzzed images. Pin library/tool identity in method provenance if necessary for offline replay.
- Keep capture and analysis off measurement-critical intervals unless the current stimulus explicitly requires them.

**Acceptance:** golden Vela/Plasma images, all scale/state/toolkit families, raw archive replay, tampered original/crop failures and malformed-input differential tests. Count PNG decode calls and critical-path wait reductions separately: decoding outside the critical path may lower CPU usage without shortening a campaign. Do not add a new decoder dependency unless a measured net gain justifies it.


### T4 — Reorder P4 scaling matrix by scale, not by family

**Current:** Native runs five scales × five states, then each supported toolkit runs five scales × five states: 25 output applications where all five families are supported.

**Proposed loop:**

```text
freeze corpus inventory/capability and exact global schedule
for scale in fixed_scales:
    apply and verify output scale once
    for family in [native-pattern, qt-wayland, gtk-wayland,
                   browser-wayland, qt-xwayland]:
        if declared unsupported:
            record skipped family cases with explicit reason
            continue
        for state in fixed_states:
            launch one fresh owned client
            apply exact native actions; capture full output
            validate source, geometry, original PNG and receipts
            terminate/reap client
finally:
    restore outputs and verify physical restoration
```

- Preserve each fresh client, all 125 cases when supported, binary/font/content/sandbox identity, frozen autostart/profile inventory, P4 v6 method/source binding and original screenshot/crop evidence. Publish a **new** order-versioned method with separate live/export acceptance; old v6 archives and checks remain unchanged.
- Represent the run as a **global ordered case sequence (scale → family → state)**, with per-scale `apply_outputs()` and readback receipts, explicit skipped cases, monotonic injection/source/capture links, no concurrent competing clients, and restoration on every failure.
- **Critical capability migration:** current `corpus.run()` permits a `corpus-unsupported` runtime receipt only on a family's *first* scale/state condition. Under scale-major execution define one admissible first probe for each available family (at or before its first scheduled case); preserve exact reason/identity/teardown checks. Later capability failure or partially executed family must **fail**, never be reclassified unsupported.
- Update both live and offline audits to verify the global order and skipped-case protocol; do not assume per-family contiguous evidence or rely on dictionary insertion order. Version workload/spec and immutable plan; retain untouched historical v3–v6 audit support.
- Freeze family order; analyze cache, GPU-residency, thermal and repeated-scale effects. Counterbalancing order would be a separately versioned methodological experiment.

**Potential:** up to 20 fewer scale applications per desktop/repetition if all families are supported, but unknown realized savings and possible order bias. Unlike T1/T2 stimulus-preserving wait substitution, this is a promising **critical-path structural saving** if the saved output transitions are not replaced elsewhere; report actual transition, settling, and audit costs before claiming a net benefit.


### T5 — Replace eligible fixed settling sleeps with verified readiness/stability

**Current:** P4 native waits 600 ms after launch and 700 ms after actions; corpus waits 250 ms and 350 ms. P3 also has setup/settling waits, while other intervals are mandatory workload stimuli.

**Design:**

- Before removing any sleep, produce a **per-family readiness proof table** covering native pattern, Qt Widgets Wayland, GTK4 Wayland, sandboxed Qt WebEngine, Qt Widgets Xwayland and applicable P3 transitions. Specify available configure/ack/buffer/presentation events, observable stable pixels, source identity, minimum interval, event ordering and timeout.
- Distinguish **configured**, **presented** and **visually stable**. A configure alone does not prove a displayed frame. Where the existing capture backend cannot prove the required state, retain the conservative wait rather than weaken the proof.
- Preserve P2 input pacing, P4 750 ms action minimum and 30-second idle resource window, fixed animation submissions and P3 pre-action stable state/post-action evidence window. Do not settle a visual transition **before** the intended observation of that transition.
- Define bounded deadlines, lost/reordered event policy and audit receipts in a versioned spec. Changes to stimulus or observation timing require a method version bump.

**Acceptance:** tests on fast/slow Vela and Plasma, delayed compositor presentation, missing/reordered configure, never-stable state, scale transitions and offline replay. Report observed readiness distributions; reduce only waits with explicit evidence.

### T6 — Reuse validated instrument calibration only if justified

**Current:** most non-resource blocks run presentation diagnostics and three input/capture control conditions before the actual workload.

**Design decision:** **do not skip calibration in the first optimization batch.** Record its cost and correctness contribution. Later, evaluate a fast per-session requalification plus a complete session-local calibration only where the spec and fault tests establish equivalent validity.

- Never reuse calibration evidence across a changed session, candidate binary, physical output, clock domain, input identity, capture backend, GPU device, or monitor condition without requalification.
- Preserve the 40 ms injected-defect sensitivity demonstration, capture coverage, distinct frame ID checks, and exact binding to the measured session.
- Any qualified reuse is a new method version, never an invisible runtime flag. Continue to reject old archives as if they satisfied new requirements.

### T7 — Persistent campaign-level takeover (separate high-risk architecture project)

**Current:** `campaign.execute()` invokes a complete supervised pilot per block. This restores and reacquires the machine between phase/target/repetition blocks, not merely between campaigns.

**Potential design:** a root-owned *campaign supervisor* acquires the machine once, while each block remains an isolated, independently supervised child with its own fresh benchmark UID profile, real logind/PAM session, measured workload, evidence, hard deadline, and checkpoint. The campaign supervisor retains original recovery intent and performs final host/display-manager restoration even when a child is killed or hangs.

**Strict requirements:**

- **Failed-block contract before fast-path efficiency:** #109 now continues after a failed block only when the *complete existing host restoration* is verified, including worker stop, physical-input release after worker exit, reserved UID/runtime teardown, conditions identity and unchanged boot. A future campaign-level supervisor must fall back to this full restoration and end the persistent takeover on failure; any subsequent block must reacquire from a verified clean host. Do not treat an in-takeover child restart, a successful endpoint check, or a still-masked display manager as equivalent to the existing continuation receipt. Never continue on cancellation, integrity/recovery failure, or changed conditions.
- Preserve attempt IDs, failure evidence, nonzero `completed-with-failures` outcome, retry-unsuccessful-only resume and independent archived audits even when later blocks succeed; normal success and recovered failure must remain distinguishable.
- No persistent compositor or reused graphical session across independent blocks; cleanup of all owned UID processes and user-manager state is proven before the next block.
- PID 1 and immutable on-disk state must enforce a campaign-level recovery budget and individual block timeouts; the current 600 s pilot runtime limit cannot just be removed without a replacement watchdog model.
- Preserve VT reservation, physical evdev input isolation and its immediate PID 1 failure path, profile/autostart inventory and code identity binding, authentication-quiescence ordering, original brightness/power/output restoration, greeter state, abort/hotplug handling, owner/root privilege separation, resume semantics, and administrator masks.
- A partially completed campaign may resume only after a verified restored host state; committed blocks remain independently re-auditable.
- Any failed boundary must restore the machine rather than continue to the next block with uncertain state.

This is a **distinct milestone** after lower-risk work and physical qualification, not a prerequisite for T1–T5. It likely offers substantial lifecycle savings but carries the highest reliability risk.


## 5. Measurement methodology and safety gates

### Baseline comparison

Measure separately:

1. `run --phase responsiveness --phase motion --phase slow-apps --phase scaling --phase overhead --target vela --target plasma --fast`
2. The same five phases with normal three-repetition policy.
3. `observe --phase responsiveness --phase motion --phase slow-apps --target vela --target plasma --fast`, separately; P3 is not in the ordinary `run` selection.

Use identical installed target binaries, refresh/mode, GPU driver, energy/brightness policy, input devices, frozen autostart inventories/profile policies, capture backend identity, and comparable temperatures/background load. Freeze the **exact current parent commit and P2 v3 / P3 v3 / P4 v6 methods** in both sides of an A/B baseline: never pool runs from older parent revisions as if they measured the same stimulus or controls. Archive all accepted, invalid, cancelled, `completed-with-failures`, restoration/continuation and retry results. Virtual/headless development checks do not qualify real-TTY performance or restoration.

**Pre-register quantitative gates before inspecting each candidate.** Two tiers avoid exhausting real machines on weak ideas while keeping confirmed claims rigorous:

**Tier A — engineering screening (exploratory):**

- Use the complete, unchanged workload for the **affected phases**, with real-TTY checks and raw evidence; `--fast` can bound screening cost but still performs one complete repetition. Do not manufacture shorter scenes/actions to obtain faster screening results.
- Prefer paired, order-rotated A/B runs; one or two ABBA quartets per affected scenario can identify obvious regressions and approximate critical-path savings. Record all attempted runs, including failures. These samples produce **exploratory estimates only**, not confirmed speedup or reliability claims.
- Promote a candidate when measured critical-path savings plausibly cover implementation/maintenance cost, with no fault-coverage regression. A ≥20% improvement in a small stage is a useful investigative signal **only if** its actual critical-path weight predicts an aggregate benefit; it is not an alternative end-to-end success gate.
- Start with a basic A/A perturbation check of the timing instrumentation where practical, before trusting small differences. The full suite need not be rerun after every trivial documentation/refactoring step.

**Tier B — independent whole-campaign validation (confirmatory):**

- A **whole campaign** is one experimental unit. Three internal repetitions are **not** three independent wall-time observations. Keep the default three-repetition scenario, the `--fast` scenario and separately selected P3 `observe` scenario distinct; validate only the scenarios for which a claim is made.
- Target at least **four ABBA quartets** *per claimed scenario* (8 baseline and 8 candidate campaigns), alternating the starting variant across quartets and extending when variance is high. This is an intentionally expensive confirmation target, not a prerequisite to low-risk code review. If impractical, publish descriptive/exploratory data instead of asserting a statistically confirmed speedup.
- Report baseline/candidate median and range, order-adjusted percentage change, and a **95% confidence interval** obtained from a prespecified *campaign-level*, order/block-aware resampling method. Preserve campaign pairing/ABBA grouping; do **not** bootstrap individual samples inside a campaign as independent observations. A confidence interval crossing zero is inconclusive; small samples cannot guarantee sufficient power.
- **Proposed meaningful-speedup claim gate:** ≥5% median reduction in accepted end-to-end duration with a 95% interval lower bound above zero, alongside passed correctness/fault gates. A smaller benefit may still justify a low-risk change on engineering grounds, but must not be advertised as meeting this threshold.
- Report failure/abort rate, invalid blocks, retry count, fault causes, recovery duration, **total operator wall time consumed per accepted complete campaign including failed attempts/retries**, and successful-only elapsed time separately. Define the aggregate ratio as total elapsed operator time divided by complete accepted campaigns (and report `N=0` as no completion, not a finite duration). Do not silently exclude failures or recovery time. Small samples cannot statistically establish an unchanged fault rate; injected-fault coverage and absence of unexplained new regressions remain mandatory.

**Method-change boundary:** T4 or T5 may legitimately create a new workload version. In that case compare overall **time-to-complete** operationally, but do not claim strict within-method workload equivalence or unchanged scientific interpretation without separate qualification. Keep the original method's archives replayable.

### Success and equivalence gates

- **Primary:** accepted START-to-confirmed-host-restoration wall time. Across reboot/resume, use attempt-local monotonic durations plus separately measured real recovery elapsed time; never subtract unrelated boot-local clocks.
- **Secondary:** stage critical path, **gross overhead eliminated versus pacing absorption versus net critical-path saving**, backend capture vs encoding/IPC analysis, process spawns, CPU overhead, readiness distributions, attempts per accepted campaign and failure/recovery cost.
- **Work equivalence:** identical actions, frames, captures, original evidence and workload versions (or explicitly new versions), fixed load/idle windows and required conditions.
- **Temporal equivalence:** record actual P2 injection intervals, P3 capture-to-input and drag trajectory receipts, P4 state-to-capture intervals; check a baseline-frozen minimum interval, jitter/late-deadline envelope and no-burst rule. Timed receipts, not nominal `sleep()` values, are evidence. An intentional stimulus change requires a new method, not a silent optimization.
- **Observation equivalence:** same source/UID/output/frame IDs, full PNG and crop oracle, instrument coverage, no unapproved capture in capture-off windows and no hidden backend fallback.
- **Reliability:** P0/P1 takeover/fault recovery, evdev grab/hotplug invalidation and independent emergency recovery trigger, frozen profile/autostart inventory, binary/hash and process/user isolation, display-manager quiescence, post-worker input-release and reserved-UID teardown receipts, recovered-failure continuation rules, complete audit/replay, and brightness/power/outputs/VT/display-manager restoration. Any uncovered mid-window interval invalidates the block.
- Do not require identical noisy physical p50/p95 latency values; investigate systematic measurement/observer bias and distributions, not bit-for-bit performance timing.

### CI and qualification

- Unit tests for boot/clock-domain accounting, overlapping/incomplete stage events, immutable timing/spec envelopes, fault invalidation and historical archive audit.
- Private Vela/KWin integrations across all P4 families including browser sandbox and Xwayland, with capture timing and input pacing proofs.
- Fault tests for session/VT loss, monitor death/freeze and event loss, user-manager exit, hotplug, stale/duplicate capture, malformed PNG, incomplete corpus, controller death and interrupted checkpoints. Add recovered block failure followed by a clean independent block, failed recovery that must stop, cancellation that must stop, post-worker input release, changed autostart/profile inventory, unauthorized process membership, concurrent display-manager login and changed/invalid evidence that must reject continuation.
- Physical fast Vela/Plasma acceptance before support or speedup claims; normal repeated acceptance remains a separate operator qualification.
- No runtime optimization may relax checks, omit evidence or convert an unsupported outcome into a pass. A correct but low-benefit track may be abandoned.


## 6. Proposed implementation order

| Stage | Scope | Output / merge gate |
| --- | --- | --- |
| 0 | Timing instrumentation and baseline | Boot-scoped critical path, **gross/absorbed/net time ledger**, current P2 v3 / P3 v3 / P4 v6 timing receipts, per-attempt recovered-failure/continuation costs, preregistered two-tier A/B design and recovery evidence |
| 1 | **T3a: simple redundant-work removal** | Same-stage decoding/metric reuse and only proven non-methodological spawn savings; independent raw audit |
| 2 | **T1a: cheap existing-path checks** | **Per-invariant feasibility/coverage matrix first**; deep attestation plus proven continuous coverage, injected faults, preserved feedback-gated cadence; no unsupported replacement of safety checks |
| 3 | **T2: persistent capture worker** | Stable per-request provenance/timestamps, original PNG/frame audit, unchanged or explicitly versioned P3 action pacing |
| 4 | **T4: scale-major P4** | New global ordered spec, capability/unsupported migration, 125-case audit where supported, old archive compatibility |
| 5 | Review **net critical-path savings**, pacing absorption, recovery costs and complexity | Decide whether to attempt optional T3b, T5 or T1b; a stage-only percentage is not an overall speedup |
| 6 (optional) | **T3b / T5 / T1b** as separate changes | Native decoder equivalence, per-family presentation proof, or independently supervised monitor coverage |
| 7 (optional) | **T6: calibration requalification study** | Independent validity/method review before any reuse |
| 8 (optional) | **T7: campaign-level takeover** | New PID 1 recovery architecture, physical acceptance under injected faults |

A milestone is not a mandate. Freeze/replay the baseline between stages; stop if complexity no longer pays. Do not combine capture, monitoring, watchdog and methodological changes in one unreviewable jump. A changed stimulus is a new benchmark method, not an unqualified harness-only speedup.


## 7. Rejected shortcuts

- Fewer actions/frames/captures or one repetition disguised as normal mode.
- Claiming temporal equivalence from identical counts while input or P3 capture-driven action cadence changes.
- Accelerated catch-up input bursts after a delayed deadline or issuing P2 input before the prior frame's presentation acknowledgement.
- Eliminating original full-output PNGs, hashes, independent crop/oracle validation or offline audit.
- Dropping mandatory idle/active windows, or parallel benchmark sessions on the same machine.
- Background decoding that contaminates active resource/latency measurements.
- Reusing desktop sessions/profiles across blocks merely to avoid setup.
- Treating configure as proof of presentation; blindly reducing sleeps.
- Lost monitor events, uncovered health intervals, reduced emergency abort or unbounded recovery.
- Comparing legacy process-start-inclusive capture duration with new in-process latency without semantic separation.
- Hiding failures/retries when comparing time per accepted campaign, or treating three repetitions within one campaign as independent experimental trials.
- Subtracting `CLOCK_MONOTONIC` timestamps from different boots.
- Reusing incompatible historical data as passing current-candidate evidence.


## 8. Open design questions and decision log

**Decisions established by this revision:**

1. Stage 0 must baseline *work, temporal pacing, observer behavior and failure costs* before changing T1/T2; boot IDs prevent cross-reboot monotonic arithmetic.
2. Success means **net elapsed time per accepted campaign**, not merely fewer subprocesses, lower CPU, or a faster stage whose savings are absorbed by preserved pacing.
3. Implement T3a before adding a native decoder. Evaluate T1a within existing supervision before considering T1b; require the T1a feasibility/coverage table before substituting any deep check.
4. P3 capture-driven actions and P2 **feedback-gated** `health()`/presentation pacing are methodological hazards: hold the frozen temporal contract or explicitly version the stimulus.
5. A scale-major P4 audit needs a global expected case order, per-scale output evidence and a strictly controlled first-family unsupported receipt.
6. Configure-only is never sufficient proof of a presented/stable image. Unproven waits stay.
7. Use a two-tier testing protocol: affordable exploratory screening of complete affected workloads, then independent whole-campaign ABBA confirmation for any quantitative speedup claim. A 20% stage gain is not a substitute for demonstrated aggregate benefit.
8. Count failed attempts/retries and recovery in operational cost; never combine monotonic timestamps across boots.
9. Parent #109 at `8ff6b83` introduces verified continue-after-restoration for failed independent blocks, frozen quiet-reference profiles, stronger identity binding and immediate input-monitor recovery. T1/T7 must preserve these, and Stage 0 must separately price the continue/reacquire path.
10. The baseline method contract is P2 v3, P3 v3 and P4 v6. Existing P3 within-audit detector caching and PNG hash validation are not new T3a work. Update this snapshot for any later parent change before authorizing code.

**Questions to resolve before corresponding implementation:**

1. For each T1a mutable condition, what **authoritative loss-detectable event source** and fallback attestation prove uninterrupted coverage? Which invariants cannot be safely optimized without T1b, and which must retain the existing deep check?
2. What exact P2/P3 **feedback-gated** pacing envelopes, deadlines and overrun policy do frozen physical Stage-0 traces support for each installed desktop? How much eliminated time would simply become controlled waiting?
3. Which Vela/KWin capture timestamps and frame identity receipts can be preserved across an in-process worker without changing the meaning of a measurement?
4. How do new first-probe/unsupported receipts and the global P4 case sequence bind to immutable plans while historical readers remain unchanged?
5. Which toolkit families have sufficient presented/stable evidence to replace fixed waits, and which retain existing waits?
6. What minimum physical fault coverage, independent watchdog, **failure-triggered full restoration/reacquisition**, and recovery/continuation proof would justify the optional persistent campaign takeover?
7. How much Stage-0 time is spent on new reference-profile generation/inventory, DM quiescence and verified post-failure continuation? Which of those costs can safely be reduced without reusing a mutable authorization across attempts?

**Decision requested before code work:** accept the revised stage order, temporal-equivalence contract, baseline protocol and fail-closed gates. The PR remains design-only; no benchmark behavior changes are introduced here.

