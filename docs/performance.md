# Performance claims and reproduction

Vela measures *observable* behavior rather than treating low memory use or high frame rate as substitutes for visual quality. This page explains the scope of the numbers in the [README](../README.md#performance).

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

## Frame timing and sharpness

- GPU readiness: see the slow-GPU scenario in [testing.md](testing.md#slow-apps) and the setup in [renderer.md](renderer.md#73-client-buffers-and-synchronization). The automated tests tolerate limited missed vblanks; **0 observed** is not a universal guarantee.
- Pixel correctness: `ctest --test-dir build -R '^sharpness' --output-on-failure` and [sharpness methodology](testing.md#sharpness-testing) exercise buffer-to-screen geometry and expected pixels. They do **not** benchmark optical text rasterization quality, icon rendering quality, or comparisons to KDE/GNOME.
- Comparative visual quality is a separate future benchmark. No unrun comparison is claimed here.

For methodology and the already archived CPU scene-diff benchmark see [scene-diff-benchmark.md](scene-diff-benchmark.md).
