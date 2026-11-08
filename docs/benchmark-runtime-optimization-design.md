# Benchmark runtime optimization — design proposal

**Status:** Draft / design-only; no implementation authorized by this document  
**Parent:** #109 (`feat/benchmark-suite`)  
**Target:** reduce end-to-end wall-clock runtime while preserving meaningful benchmark workloads, evidence, isolation, and recovery  
**Scope:** the installed Vela/Plasma benchmark paths currently implemented in #109; no change to supported desktops

## 1. Problem and goals

The benchmark runner in #109 prioritizes complete, reproducible evidence and supervised physical-machine recovery. That is the right default, but its elapsed time includes significant work unrelated to the measured desktop behavior: repeated session takeovers, per-action host queries, process creation for capture, repeated PNG decoding, repeated output-scale configuration, and fixed settling intervals.

This proposal treats **time spent measuring the system** and **time spent operating the harness** separately. The objective is to reduce the latter before changing any statistical workload.

### Goals

1. Reduce **total elapsed campaign time**, measured from the accepted START to verified return of the machine, for both `--fast` and the default three-repetition mode.
2. Preserve *the same* per-condition input actions, submitted frames, fixed work, screenshots, independent repetitions, and accepted source evidence unless a separately reviewed/versioned methodological change is explicitly justified.
3. Reduce observer/interference overhead in measured windows, particularly repeated external process and D-Bus activity.
4. Preserve the real-TTY isolation model, fresh desktop profiles and sessions, crash/hang/hotplug recovery, original evidence, offline re-audit, and fail-closed behavior.
5. Make actual savings measurable, attributable, and repeatable.

### Non-goals

- Making `--fast` use fewer actions or frames: it must continue to mean one *complete* independent repetition.
- Changing 100 measured input actions, 600 measured animation submissions, the eight P2 loaded/control conditions, 12 P3 captures per action, 125 P4 scaling cases (where fully supported), the 30-second resource idle interval, 1,800 active-animation submissions, or 12 window maximize/restore cycles as a shortcut.
- Replacing or weakening raw evidence, hashes, source-identity checks, pixel oracles, sandbox policies, original capture backends, or cleanup requirements.
- Parallel benchmark sessions, simultaneous desktop comparisons, or overlapping workloads that would interfere with resource/latency measurements.
- Claiming a numerical speedup without timing a real accepted campaign.

## 2. Baseline and concrete observations

The existing code establishes the following **work counts**, not actual elapsed durations:

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

**Important:** The sum of fixed sleeps is only a lower bound for those explicit waits, not an estimated achievable saving. `model.BUDGETS` and campaign's printed duration ranges are planning allowances, not measured per-stage costs; systemd deadlines are upper bounds, not automatic delays. P3 `observe` is a separate selection, not part of the ordinary five-phase `run` matrix.

## 3. First requirement: profile the harness before optimizing

Implement low-intrusion, monotonic, structured **harness stage timing** *before* implementing performance changes. Each stage record should contain:

- Campaign ID, block ID, attempt, target, repetition, method/spec, stage name, start/end `CLOCK_MONOTONIC` timestamps, status, and (when applicable) subprocess exit and instrument identity.
- Explicit categories: preflight/context refresh, greeter/VT setup, display-manager stop/start, profile reset, session launch/readiness, condition apply/verify, presentation diagnostics, input/capture calibration, fixture launch, per-condition settling, measured workload, capture transport, image analysis, evidence hashing/audit, fsync/checkpoint, session teardown, and recovery.
- Separate **measured workload windows** from **harness and validation work**. Do not add heavyweight tracing, polling, hashing, printing, or image processing *inside* the timed path simply to profile it.
- A summarized critical-path view by phase/target/repetition, with the raw stage events retained and re-checkable. A separate CPU-time and process-spawn count is useful where available.
- On cancellation or failed blocks, preserve partial stage timing without marking incomplete measurement evidence as accepted.

Measurement: compare a frozen pre-optimization baseline with candidate builds on the same hardware and conditions; repeat and rotate candidate ordering to reduce temperature/cache/order effects. Report median/range of *whole-campaign* wall time and per-stage costs, alongside failures, measurements, and environment identities. Do not infer gains from the planner budgets.

## 4. Proposed optimization tracks

### T1 — Split inexpensive liveness checking from deep session attestation

**Current hot path:** P2 `run_input()` calls `health()` for every input action. The present `health()` performs full `verify_session()`, which includes external `loginctl`, `systemctl`, `busctl`, namespace reads and `fgconsole`. P2 slow-apps alone repeats at least 840 action-level health checks per block.

**Design:**

- Introduce a scoped, root-owned monitor for each benchmark block, with explicit monitor readiness, checked identity, bounded freshness, immutable session/VT identifiers, and a fail-closed `healthy`/invalidated state.
- Deep attestation remains mandatory before the measurement window, after it, at key transitions, and at final acceptance. Monitor failures, loss of heartbeat, session/VT change, input hotplug, unexplained new processes, and evidence gaps invalidate the affected block.
- In the action loop, read a low-cost state maintained by that monitor instead of synchronously launching multiple utilities. Establish an independent mechanism to detect mid-window changes (event subscription, guarded periodic revalidation, or equivalent); **checking only at the beginning and end is insufficient**.
- Keep evdev exclusive-grab enforcement and the emergency abort path independent of the optimization; never suppress an abort to improve timing.
- Explicitly capture any detection latency in evidence; monitor CPU/process overhead must be characterized. The monitor must not silently survive into resource-accounting scopes in a way that makes desktop comparisons asymmetric.

**Acceptance:** fault injection for loss of user manager/session, VT switch, monitor death/freeze, hotplug, stale heartbeat, and mid-window invalidation; no accepted results after an unobserved interval. Identical input/motion sample requirements. Compare runtime and measured interference before/after.

### T2 — Use persistent, serialized screenshot transport in a block

**Current hot path:** P3 calls the capture worker repeatedly, which in Vela currently invokes `vela-screen-dump` then `vela-bench-desktop --analyze` per capture; P4 also executes one-shot capture requests.

**Design:**

- Evaluate a block-scoped capture service/worker that holds the correct compositor-specific capture connection and accepts individual serialized capture requests. Keep the existing `ext-image-copy-capture-v1` (Vela) and KWin Screenshot2 (Plasma) semantics; no portal/software/fallback substitution.
- Each request must independently record begin/end bounds, backend, output ID and geometry, source receipt, and an original full-output PNG digest. Preserve existing capture counts, ordering, separation, and per-request auditable evidence.
- Do not run capture during the P2 neutral measurements or P4 resource measurements that explicitly disable it.
- Keep capture requests synchronous with benchmark actions where the current action model depends on a prior screenshot. Any asynchronous analysis runs only outside relevant measured intervals or in an isolation domain shown not to perturb them.
- Failure of the worker, missing response, buffer reuse error, frame-order ambiguity, or malformed image invalidates the block; recreate workers after session resets.

**Acceptance:** replay of original PNG/source validation; exact frame count and order; backend/UID/output identity checks; failure and restart tests; before/after capture request/receipt distributions (not conflated with desktop responsiveness).

### T3 — Make PNG analysis native/efficient without changing the pixel oracle

**Current hot path:** `pixels.png_rgb()` performs PNG filter reconstruction in Python, including per-byte filtered scanlines. Corpus live checks can decode the same original image several times; P3 audits already cache byte-identical image decoding within an audit.

**Design:**

- First remove avoidable *same-stage* decoding with content-addressed caches and passing already validated pixel buffers to metric consumers. Never cache across a changed image/hash or different evidence identity.
- Benchmark a bounded native PNG decoder/unfilter implementation (or an explicitly pinned library with a strict validation wrapper). Continue checking magic, chunk ordering/CRC, decompressed length, image dimensions, accepted color formats, bit depth, no unsupported interlace, and unresampled RGB contents.
- The native route must produce byte-for-byte identical RGB buffers and identical derived metrics/original PNG checksums to the existing decoder for accepted inputs. Differential fuzz/malformed-input tests must establish equivalent rejection, not broaden acceptance by accident.
- Audit from persisted original files at least once independently; caching is a performance optimization, not a substitute for verifying each source file hash. Keep any new decoder version and executable/library identity in the immutable tool/method provenance as appropriate.
- Move expensive decode/metrics out of measured windows where those metrics do not gate the next action; retain fail-closed audit before declaring completion.

**Acceptance:** golden captures across both desktops, all scale/state/corpus families, malformed/truncated/oversized/CRC/filter tests, tampered original/crop tests, and exact offline replay comparisons.

### T4 — Reorder P4 scaling matrix by scale, not by family

**Current:** Native runs five scales × five states, then each supported toolkit family runs five scales × five states. `apply_outputs()` is called once per scale for each of five case families (25 applications with all four toolkits).

**Proposed loop:**

```text
for scale in fixed_scales:
    apply and read back display scale once
    for family in [native-pattern, qt-wayland, gtk-wayland,
                   browser-wayland, qt-xwayland]:
        if supported:
            for state in fixed_states:
                launch fresh owned client
                apply fixed native desktop actions
                validate source geometry and original capture
                terminate/reap client
finally:
    restore original outputs and verify restoration
```

- Preserve one new client per case, 125 distinct cases when fully supported, stable font/content/binary identity, sandbox, and original screenshots.
- Preserve the exact scale/state/family matrix and output readback. Update the offline audit to validate the **new case order**, per-scale output evidence, no concurrent competing windows, and complete restoration on every error path.
- Do not silently combine v6 evidence with reordered runs. Introduce a new, versioned workload/spec and preserve backward replay of v3–v6 archives. A sequence change is a method change even if the tested matrix is unchanged.
- Consider order/carry-over effects: family order and repeated-scale exposure can affect caches. Compare paired raw measurements, and consider counterbalancing family order only if its reproducibility and spec semantics can be frozen.

**Potential effect:** up to 20 fewer scale applications per desktop/repetition where every family is supported; savings are unknown until measured.

### T5 — Replace eligible fixed settling sleeps with verified readiness/stability

**Current:** P4 native has 600 ms after each client launch and 700 ms after each state action; corpus has 250 ms and 350 ms respectively. Other parts of P3 and input calibration use bounded fixed settling intervals.

**Design:**

- Where the benchmark already records configure/state events, require the expected configure, buffer/viewport state, and (where the metric needs pixels) a matching captured/presented state instead of assuming a fixed delay means settled.
- Define event sequence, stabilization condition, monotonic timestamp proof, maximum timeout and failure policy in the versioned spec. For tests of visual transitions, do not wait for the final state *before* the observation window or accidentally remove the transition being measured.
- Keep minimum intervals that are actual workload stimuli (e.g. P2 input pacing, P4 750 ms window-action minimum), and mandatory elapsed measurement windows (30 s idle). These are **not** arbitrary setup sleeps.
- A protocol event alone may not prove that the compositor completed presentation. Where presentation/stability cannot be verified with the declared backend, retain the conservative wait rather than claim it is safe to remove.
- Preserve the first pre-action stable state requirement and the post-action evidence window in P3.

**Acceptance:** fast and slow desktop environments, delayed compositor presentation, lost/misordered configure events, never-settling states, output scale changes, and replay of valid/invalid observations. Report observed readiness times as a distribution.

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

- No persistent compositor or reused graphical session across independent blocks; cleanup of all owned UID processes and user-manager state is proven before the next block.
- PID 1 and immutable on-disk state must enforce a campaign-level recovery budget and individual block timeouts; the current 600 s pilot runtime limit cannot just be removed without a replacement watchdog model.
- Preserve VT reservation, physical evdev input isolation, original brightness/power/output restoration, greeter state, abort/hotplug handling, owner/root privilege separation, resume semantics, and administrator masks.
- A partially completed campaign may resume only after a verified restored host state; committed blocks remain independently re-auditable.
- Any failed boundary must restore the machine rather than continue to the next block with uncertain state.

This is a **distinct milestone** after lower-risk work and physical qualification, not a prerequisite for T1–T5. It likely offers substantial lifecycle savings but carries the highest reliability risk.

## 5. Measurement methodology and safety gates

### Baseline comparison

Measure separately:

1. `run --phase responsiveness --phase motion --phase slow-apps --phase scaling --phase overhead --target vela --target plasma --fast`
2. The same five phases with the normal three-repetition policy.
3. `observe --phase responsiveness --phase motion --phase slow-apps --target vela --target plasma --fast`, separately, because P3 is not part of the same `run` selection.

Run paired baselines/candidates with identical installed desktop binaries, refresh/mode, GPU, energy/brightness and thermal conditions where practical, rotating execution order. Preserve all archives. A prototype or virtual compositor may help develop the changes, but it does **not** qualify real-TTY time savings or physical recovery.

### Success measures

- **Primary:** accepted end-to-end elapsed wall time, including setup, auditing, cleanup and confirmed machine restoration.
- **Secondary:** per-stage wall time, capture request duration and cadence, external process count, CPU-time overhead, repetitions/accepted blocks per campaign, time in idle versus measurement, and failure/abort rate.
- **Scientific invariants:** unchanged counts of frames/actions/cases/repetitions, unchanged capture requirements, valid p50/p95 measurements and clock-domain checks, intact load-window/achieved-load checks, identical accepted pixel-oracle semantics, and no hidden backend fallback.
- **Reliability invariants:** all P0/P1 takeover/fault recovery checks, real session/UID/process isolation, input grabs and hotplug invalidation, desktop teardown, restored output/brightness/power/VT/display-manager state, accepted offline raw replay.
- Do not mandate identical latency percentiles across independent noisy physical runs. Investigate systematic bias and any collector-induced interference; compare methodology, trace and distribution rather than requiring bit-for-bit timing equality.

### CI and qualification

- Unit tests for versioned plans, immutable spec binding, stage accounting, and compatibility with historical archive readers.
- Private Vela/KWin integration tests for T1–T5; corpus fixtures include native Wayland, GTK, Qt WebEngine with sandbox, and Xwayland.
- Fault tests for worker crashes/hangs, stopped user manager, capture worker death, monitor freeze, unexpected input device, VT loss, malformed and duplicated PNG evidence, incomplete corpus, and interrupted checkpoints.
- Physical fast acceptance for current installed Vela/Plasma candidates before claims of support; three-repetition acceptance remains a separate operator decision.
- No runtime optimization should relax a failing acceptance check, increase missing evidence or hide unqualified conditions as 'passed'.

## 6. Proposed implementation order

| Stage | Scope | Output / merge gate |
| --- | --- | --- |
| 0 | Timing instrumentation / baseline | Stage breakdown on real accepted runs; minimal perturbation demonstrated |
| 1 | T1 low-cost, fail-closed health monitor | Fault-coverage proof + reduced action-loop overhead |
| 2 | T3 efficient PNG analysis and redundant-decode removal | Exact decoder/oracle equivalence; no lost raw checks |
| 3 | T2 persistent serialized capture transport | Original PNGs + unchanged per-capture timing and order receipts |
| 4 | T4 per-scale corpus schedule | Version bump, 125-case evidence and historical audit compatibility |
| 5 | T5 event-driven settling only where provably equivalent | Explicit state/presentation proof and timeout behavior |
| 6 | T6 requalification study (optional) | Independent method/validity review before any behavior change |
| 7 | T7 persistent takeover (optional, separately qualified) | New supervisor/recovery architecture and physical fault acceptance |

These are milestones, not a mandate to implement every track. Review measurements after each stage and stop when further complexity no longer pays for itself. Isolate changes in reviewable commits; do not mix capture, watchdog and methodological changes in a single untestable jump.

## 7. Rejected shortcuts

- Fewer actions/frames/captures or one repetition disguised as normal mode.
- Eliminating full-output PNGs, hash checks or pixel validation.
- Dropping or shortening mandatory idle/active measurement windows.
- Running comparisons in parallel on the same GPU/desktop or allowing background decode work to contaminate an active resource measurement.
- Keeping the same user session/profile between blocks merely to avoid setup.
- Blindly lowering sleeps without source/presentation readiness evidence.
- Longer unbounded watchdog deadlines or dropping emergency recovery.
- Reusing older data/conditions across incompatible versions as if it were a passing current-candidate benchmark.

## 8. Open design questions

1. How can the T1 monitor prove there were **no** missed mid-window changes despite event loss, scheduling stalls or monitor death, while itself adding less noise than deep queries?
2. Can a persistent capture worker retain the exact current Vela/KWin capture APIs and independently timestamp requests without changing frame semantics?
3. Should PNG optimization be a tightly wrapped native decoder or first just a zero-new-dependency change to caching and repeated decoding?
4. Does scale-major execution introduce measurable cache/order bias relative to the existing family-major sequence, and should we fix or counterbalance order?
5. What minimum proof of state stability is actually available for each P4 family, and which waits must remain because configure alone is insufficient?
6. What physical fault coverage and watchdog design would justify moving from block-level recovery to campaign-level recovery?

**Decision requested before code work:** agree on stage ordering and safety gates. This document intentionally adds no benchmark behavior changes.
