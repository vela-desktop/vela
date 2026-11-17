# Performance claims and reproduction

Vela measures *observable* behavior rather than treating low memory use or high frame rate as substitutes for visual quality. This page explains the scope of the numbers quoted in the [README](../README.md#why-vela).

The [automated benchmark suite design](benchmark-suite.md) proposes one
unattended campaign for desktop comparisons and Vela-version regressions.
It covers responsiveness, motion, slow applications, fractional scaling and
desktop overhead. P0 instrument and recovery qualification passed on the
initial Vela/Plasma machine on 2026-10-22, including independent calibrations,
coarse capture and crash/hang recovery with verified hardware restoration.
The common campaign executor and checkpoint/resume are implemented, with
an instrument-only `--exercise` mode for phase/repetition acceptance. P1's
campaign-core acceptance also passed on that setup on 2026-10-22: four blocks
across two phases in fast mode and six blocks in normal mode, with verified
restoration. P2 now implements native-client responsiveness, animation and
paired GPU-producer measurements, with warm-up exclusions, raw audits and
independent-block statistics. A six-block real-TTY P2 fast campaign passed
on that machine, including raw-data audits and restoration; normal
three-repetition acceptance was deferred by the operator. P3 adds verified
native launcher/window/desktop actions through coarse output capture, with
isolated Vela/KWin tests and initial-machine real-TTY fast acceptance passed.
P3 responsiveness and motion passed in separate campaigns with different scene
builds; these are functional checks, not repeated comparative measurements.
The GPU producer's achieved median
durations were approximately 77–89 ms rather than the requested 150 ms,
within the current declared tolerance. Full family coverage and comparative claims remain
incomplete. There are no new published comparative results.

P4 now implements native Wayland scaling-pattern captures and UID-wide
CPU/PSS collection for idle, fixed client animation and synthetic local work.
The corpus/resource-interaction coverage and physical P4 acceptance remain
incomplete. Nonzero pixel differences are measured outcomes, not instrument
failures; invalid identities, capture geometry or restoration still reject a
block. See [P4 scope](benchmark-suite.md#p4-implementation-and-acceptance-scope).

The 2026-11-01 benchmark-branch integration includes upstream `efdc9c6`.
Deferred capture and cooperative GPU-producer dispatches require fresh
instrument qualification; the older physical acceptance above does not
qualify this candidate. The [upstream integration review](benchmark-suite.md#upstream-integration-review-2026-11-03)
records what can be reused for observation, fractional scaling and session
accounting, and which timing/provenance gaps remain.

## Existing published observations

The README previously reported, from the primary development machine (Ryzen 9 9950X, Radeon RX 9070 XT, CachyOS with Mesa/RADV):

- Compositor idle: around **17 MiB PSS** and **0.1% of one CPU core**
- Shell idle: around **143 MiB PSS** and **0.1% of one CPU core**
- Files first frame: **104 ms**, median of five launches
- A deliberately slow GPU client: **47 missed vblanks before** ready-commit handling and **0 in one measured run after** it, with a 60 Hz headless output and roughly 150 ms delayed GPU work ([renderer method](renderer.md#73-client-buffers-and-synchronization))

Those are *historical reported measurements*, not new measurements made for this documentation update. The raw measurement JSON, exact original commit, Mesa version and full environment metadata are not currently archived with the observations: do not treat them as independently reproduced benchmarks.

## Run a comparable idle sample

In a running Vela session:

```sh
sh scripts/measure.sh -s 30 --json
sh scripts/measure.sh -s 30 --files --json
```

The second command also starts Files five times to estimate first-frame time. It is not a neutral idle experiment; record it separately.

Definitions from `scripts/measure.sh`:

| Value | What the script actually reads |
|---|---|
| Memory | Proportional set size (PSS) from `/proc/PID/smaps_rollup`; **not RSS**, and not total desktop/system memory |
| CPU % | Change in user + system CPU ticks, divided by elapsed sampling time and ticks/second; **percent of one logical CPU** |
| Voluntary context switches/s | Difference in `voluntary_ctxt_switches` from `/proc/PID/status` divided by sampling interval; this is a proxy, **not the number of all process wakeups** |
| Battery W | Average available battery discharge power; **whole computer**, not attributable to Vela alone |
| Files first frame | Median of up to five first-frame observations; missing samples should not be represented as successes |

For historical consumers the JSON key `wakeups_per_s` remains available, but its actual meaning is **voluntary context switches per second**. Record the number of valid first-frame samples separately when publishing comparative measurements.

## Archive a reproducible result

When publishing a number, retain the raw `--json` output with at least these metadata fields in a companion text file or manifest:

```text
vela_commit:
date_utc:
distribution_and_version:
kernel:
gpu:
driver_and_version:
mesa_and_vulkan_info:
session: drm | nested | headless
outputs_and_scale:
power_source:
build_type:
test_command:
number_of_repetitions:
warmup_or_cold_start_policy:
raw_result_path:
limitations:
```

Run multiple independent samples. Preserve their individual results as well as a median and dispersion; avoid quoting a single run as a general guarantee. Test machines should not run unrelated heavy workloads during an idle measurement.

## CPU targets: controlled A/B methodology

For x86-64, the project baseline is **x86-64-v3**. A local build may use
`-DVELA_CPU_TARGET=znver5` on Ryzen 9000 / Zen 5, or
`-DVELA_CPU_TARGET=native` on the machine where it will run. These options
change only code compiled from Vela's C/C++ sources; they do not rebuild
the system's Qt, wlroots, glibc, pixman or Mesa libraries, and do not change
the SPIR-V shader compiler settings.

To evaluate the baseline against the old generic architecture, or compare
Zen 5 tuning, use separate build directories from **the exact same commit**:

```sh
cmake -S . -B build-cpu-generic -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVELA_CPU_TARGET=x86-64
cmake -S . -B build-cpu-v3 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVELA_CPU_TARGET=x86-64-v3
cmake -S . -B build-cpu-znver5 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVELA_CPU_TARGET=znver5
cmake --build build-cpu-generic --parallel
cmake --build build-cpu-v3 --parallel
cmake --build build-cpu-znver5 --parallel
```

The generic target is **measurement-only and unsupported for normal use**.
The `znver5` target requires a supporting compiler (for example GCC 14 or
newer) **and** a Zen 5 CPU; CMake validates compiler acceptance, not whether
the machine running the resulting executable supports its instruction set.
Never distribute a locally specialized binary as a standard v3 package.
On Arch local package builds, set
`VELA_CPU_TARGET=znver5 makepkg -si` (or use `native`), rather than relying
only on `makepkg.conf` CFLAGS: Vela deliberately applies its CPU target last.

Measure the `vela-scene-diff-bench` executable **from each build directory**
(method in [scene-diff-benchmark.md](scene-diff-benchmark.md)); a microbenchmark
does not require a running Vela session. For compositor/shell measurements,
`scripts/measure.sh` measures an **already running** Vela session, not
binaries in a build directory. For each candidate, first stop the previous
candidate and launch the selected build, for example, in another terminal:

```sh
VELA_STATS=1 scripts/run-nested.sh build-cpu-v3
# Repeat separately for build-cpu-generic and build-cpu-znver5.
```

Before measuring, identify the compositor and shell PIDs of that candidate
and verify both `readlink -f /proc/<pid>/exe` paths against the corresponding
`build-cpu-v3/compositor/vela-compositor` and
`build-cpu-v3/shell/vela-shell` paths (or the selected build directory).
Do not rely on `pgrep` order or measure the previously installed desktop.
Use the candidate's `WAYLAND_DISPLAY` (from its shell process environment)
to disambiguate multiple sessions, then run:

```sh
scripts/measure.sh -d <candidate-WAYLAND_DISPLAY> -s 30 --json
```

Avoid `--files` unless `vela-files` also resolves to the matching build;
that option otherwise uses the executable from `PATH`. Run the compositor
and shell under the same workload for every target, testing unchanged scenes,
heavy window movement/restacking, popup churn, and multi-output loads.
Alternate run order, warm caches and repeat samples. **Archive each result
with its `VELA_CPU_TARGET`, build directory, verified executable paths,
CMake build type, compiler and flags, Git commit, CPU affinity, governor,
GPU driver and power profile**; the current `measure.sh` JSON does not
embed the CPU target automatically. Assess median and tail frame times, missed
vblanks, input latency, CPU, memory and binary size. A faster microbenchmark
does **not** establish a smoother desktop, so do not claim a gain before
results are archived.

## Frame timing and sharpness

- GPU readiness: see the slow-GPU scenario in [testing.md](testing.md#slow-apps) and the setup in [renderer.md](renderer.md#73-client-buffers-and-synchronization). The automated tests tolerate limited missed vblanks; **0 observed** is not a universal guarantee.
- Pixel correctness: `ctest --test-dir build -R '^sharpness' --output-on-failure` and [sharpness methodology](testing.md#sharpness-testing) exercise buffer-to-screen geometry and expected pixels. They do **not** benchmark optical text rasterization quality, icon rendering quality, or comparisons to KDE/GNOME.
- Comparative visual quality is a separate future benchmark. No unrun comparison is claimed here.

For methodology and the already archived CPU scene-diff benchmark see [scene-diff-benchmark.md](scene-diff-benchmark.md).
