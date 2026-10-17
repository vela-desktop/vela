# CPU scene comparison benchmark

`vela-scene-diff-bench` measures the actual `diff_with_last()` in
`compositor/src/scene/frame.c`, including damage-region updates. It needs
wlroots and pixman but no display, running compositor, or GPU. The test driver
includes the private implementation and discards unrelated rendering sections
at link time; it replaces only the surface-forget callback with a counter.
Timed scenes contain no surfaces, so that callback does not run.

The two element lists are built before measurement and alternate as the real
function swaps current/last. Scenarios cover unchanged scenes, movement,
one-position rotations, reversed order, replacement of one quarter of the
elements, and complete replacement. Elements occupy a grid of 16×16 boxes.
Output counts simulate independent comparisons in sequence, without the rest
of a multi-monitor compositor.

Each invocation warms up, chooses batches lasting at least approximately
2 ms (up to 65,536 ticks), and measures 21 batches with `CLOCK_MONOTONIC`.
A tick compares every configured output and clears its damage ring before
comparison. Reported median and p95 are **batch-average time per tick**, not
individual-frame latency percentiles. The Python runner saves raw samples at
the invocation level as CSV, reports the median of three invocation medians,
and alternates candidate/reference order between repetitions.

## Reproduce

From the repository root, retain the pre-change source without changing the
checkout. The optional reference source must remain compatible with the current
headers; revision `7977c1b` is the baseline for this scratch-buffer change.
Both benchmark binaries use the same fixture, current header layout, compiler
flags, libraries and search algorithm.

```sh
mkdir -p build-scene-bench
git show 7977c1b:compositor/src/scene/frame.c > build-scene-bench/frame-reference.c
cmake -S . -B build-scene-bench \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DVELA_BUILD_COMPOSITOR=ON -DVELA_BUILD_TOOLS=ON \
  -DVELA_BUILD_SHELL=OFF -DVELA_BUILD_POLKIT=OFF -DVELA_BUILD_REPORT=OFF \
  -DVELA_SCENE_DIFF_REFERENCE="$PWD/build-scene-bench/frame-reference.c"
cmake --build build-scene-bench --target \
  vela-scene-diff-bench vela-scene-diff-reference vela-scene-diff-test -j4
ctest --test-dir build-scene-bench -R '^compositor.scene-diff$' --output-on-failure
python3 compositor/tests/bench_scene_diff.py \
  build-scene-bench/compositor/vela-scene-diff-bench \
  --baseline build-scene-bench/compositor/vela-scene-diff-reference \
  --cpu 0 --csv build-scene-bench/scene-diff-comparison.csv
```

Choose an allowed CPU for `--cpu`, or omit it to allow normal scheduling.
Default element counts are 0, 8, 32, 128, 512 and 1,024, with 1, 2 and 4 outputs
and three repetitions: 648 invocations across both binaries. `--counts`,
`--outputs` and `--runs` select smaller or larger sweeps. A single case is also
available directly, for example `vela-scene-diff-bench 128 2 reverse`.

## Scratch buffer

Each output retains its match flags, grows them with the existing `vela_grow`
helper and clears only the used portion. There is no match-buffer allocation or
free at steady scene size. Empty scenes need no buffer. Capacity is retained
when a scene shrinks and released when the output frame is destroyed; on this
platform 1,024 elements require 1 KiB per output. Growth follows the same
out-of-memory policy as the existing element lists.

`compositor.scene-diff` verifies damage and list swapping, changed appearance,
movement, restacking, disappearance with surface notification, empty scenes,
and shrink/regrow cycles that would expose stale match flags. Linker allocation
wrappers check zero allocations/frees during 240 warmed comparisons per
output, separate buffers for two outputs, and allocation only when capacity
grows. These counters cover the match buffer and direct executable calls;
they do not intercept allocations inside shared wlroots/pixman libraries.

## Measurement on 2026-10-09

Intel Core i3-1215U, CPU 0 pinned, default `powersave` governor; GCC 16.2.1,
`RelWithDebInfo` (`-O2 -g -DNDEBUG`), wlroots 0.20.2, pixman 0.46.4.
The allocation-based implementation was measured before applying the reuse
change. The paired sweep was then repeated with the reproducible reference
target above; [all 648 invocation results](benchmarks/scene-diff-2026-10-09.csv)
are retained. Selected medians, in microseconds per tick:

| Scenario | Elements | Outputs | Reference | Reused flags | Change |
|---|---:|---:|---:|---:|---:|
| stable | 8 | 1 | 0.095 | 0.093 | -2.0% |
| stable | 32 | 1 | 0.344 | 0.315 | -8.5% |
| stable | 128 | 1 | 1.308 | 1.301 | -0.5% |
| stable | 512 | 1 | 5.192 | 5.165 | -0.5% |
| reverse | 128 | 1 | 15.684 | 15.689 | +0.0% |
| replace-all | 128 | 1 | 25.225 | 25.375 | +0.6% |
| replace-all | 512 | 1 | 374.594 | 375.853 | +0.3% |
| replace-all | 1,024 | 1 | 1510.547 | 1474.377 | -2.4% |
| replace-all | 1,024 | 4 | 5948.065 | 5951.364 | +0.1% |

The absolute saving in the 32-element stable case is about 29 ns. Most cases
show small changes, including small regressions; the larger-case differences
are similar to run-to-run variation. This is evidence for removing steady
match-buffer allocations with little timing impact, not a substantial renderer
speedup. Completely replaced lists show approximately quadratic scaling when
the element count doubles, with much larger costs than the allocation itself.

## Limits

This benchmark excludes scene construction, occlusion, Vulkan submission and
presentation. It cannot establish frame latency, missed-vblank rate or an
improvement at 240 Hz. Large, completely replaced or heavily reordered lists
exercise the quadratic search, while stable lists normally match at the first
comparison. The search remains unchanged; these cases provide a baseline for
a separate optimization if actual scene sizes and frame measurements justify
it. Check the complete renderer on GPU hardware with `VELA_STATS=1`, comparing
the same workload and output configuration on both revisions.

## Indexed matching for structural scene changes (2026-10-17)

The scene diff still flattens and compares each output's elements each frame.
No scene graph or damage-tracking architecture has changed.

Previously every current element searched the old list circularly starting at
its expected position. Stable frames were fast, but even a single insertion
at the front could shift all subsequent positions and trigger approximately
quadratic work. The new matching logic keeps the direct same-position check
and builds a per-output hash index of the old elements **only if a key is
missing at its expected position**. Buckets and links are reused across frames.
Matching is expected O(N) for both stable and structurally changed scenes,
with hash collisions affecting constants; the construction is not performed
for stable ordered scenes. Duplicate keys use the same circular first-match
rule as before. Existing damage and restacking decisions remain unchanged.

### Additional functional and benchmark coverage

The `compositor.scene-diff` test now exercises changes to the flattened lists:

- `insert-front` and `insert-middle`: inserting an element shifts older elements
  without incorrectly damaging them.
- `remove-front`: removal reports the correct damage and surface-forget event.
- `restack-window`: raising an existing middle element tests z-order damage.
- `popup-open-close`: two temporary inserted layers appear and disappear.
- `animation-churn`: several transient layers appear, change appearance and
  disappear while the existing elements retain their order.
- Duplicate-key regression: matching preserves the original circular search's
  choice, including which old surface gets forgotten.
- Allocation regression: steady scenes retain the original allocation-free
  fast path; once warmed, structurally changed scenes reuse index buffers.

These six new scenarios are also available through `vela-scene-diff-bench`
and `compositor/tests/bench_scene_diff.py`. The historic CSV linked above
contains **only the original six scenarios and the previous implementation**;
it has not been retroactively changed. To get comparable measurements, rerun
the suite against a compatible baseline on the same CPU and build setup.
Real frame-time conclusions still require compositor/GPU measurement.
