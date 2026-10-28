# Deferred SHM capture readback (#132)

This change replaces presentation-thread waits and full-frame CPU copies for
`ext-image-copy-capture-v1` output and window SHM captures. Real AMD and NVIDIA
GPU suites must validate the exact candidate before it is considered fixed.

## What the previous PRs established

| PR | Change and outcome |
| --- | --- |
| [#111](https://github.com/vela-desktop/vela/pull/111) | Separate cached/coherent readback staging; fixed the original AMD stall. Already in main. NVIDIA still failed with those memory properties. |
| [#128](https://github.com/vela-desktop/vela/pull/128), [#129](https://github.com/vela-desktop/vela/pull/129) | GPU-suite experiments and salvaged IPC/input fixes. NVIDIA capture failed with 82/41 and 83/41 missed vblanks/captures. No capture fix. |
| [#130](https://github.com/vela-desktop/vela/pull/130), [#131](https://github.com/vela-desktop/vela/pull/131) | Separate animation sample and desktop selection work; capture remained independent. |
| [#155](https://github.com/vela-desktop/vela/pull/155) | Opt-in phase profiling and resolution/refresh experiments. GPU wait wall time was about 1.35 ms; staging-to-destination memcpy about 10 ms. Alpha fixup was inactive. Closed without merging its temporary instrumentation. |
| [#156](https://github.com/vela-desktop/vela/pull/156) | Four CPU copy workers shortened a synchronous stall, but NVIDIA still failed. The workers were removed. This PR now implements deferred completion. |

The roadmap references in #141 and #146 classify #132 as a beta blocker; they
introduce no alternative readback implementation.

## Hardware evidence before this implementation

All timings below are CPU elapsed times, not GPU transfer timestamps.

| Run | Exact source | Observation |
| --- | --- | --- |
| [NVIDIA #18](https://github.com/vela-desktop/vela/actions/runs/40209046403) | `cb421563` (#129) | 83 missed vblanks / 41 captures. |
| [NVIDIA #38](https://github.com/vela-desktop/vela/actions/runs/40490410312) | `59cc2433` (#155) | 84/40; readback wait p50 1.353 ms, combined copy p50 10.476 ms; cached/coherent staging. |
| [NVIDIA #39](https://github.com/vela-desktop/vela/actions/runs/40563783983) | `d1ada675` (#155) | 86/41; memcpy p50 9.936 ms; alpha fixup off for all 41 captures. |
| [NVIDIA #41](https://github.com/vela-desktop/vela/actions/runs/40586655037) | `2f2f7df1` (#155) | 1440p180: 81/40; 1080p180: 45/43; 1440p60: 0/37. The unchanged full suite still failed capture. |
| [AMD #43](https://github.com/vela-desktop/vela/actions/runs/40586663325) | `2f2f7df1` | Full 20/20. |
| [NVIDIA #44](https://github.com/vela-desktop/vela/actions/runs/40682239824) | `5659ba5b` (CPU workers) | A/B: serial 81/40; four workers 53/43. Strict suite 44/43, 19/20. Rejected as a fix. |
| [AMD #46](https://github.com/vela-desktop/vela/actions/runs/40682256933) | `5659ba5b` | Full 20/20. |
| [NVIDIA #45](https://github.com/vela-desktop/vela/actions/runs/40688237404) | `b677c3dd` (serial diagnostic) | Strict suite 80/41, 19/20. Extra probe: first destination copy 5.3–12.0 ms, second mapped copy about 0.85–1.10 ms; RAM copy about 0.91–1.08 ms. |
| [AMD #47](https://github.com/vela-desktop/vela/actions/runs/40688227684) | `b677c3dd` | Capture passed. Full suite failed the separate brightness burst sequence check (386/3001). |

The last probe does **not** prove permanently slow mapped GPU memory. The
second copy has different cache warmth and an already-backed destination.
Destination page faults, zeroing and cache effects remain possible contributors.
Moving only the GPU wait or only alpha conversion cannot address the measured
whole synchronous operation.

## Asynchronous boundary and ownership

The audited wlroots 0.20 paths are `types/wlr_ext_image_copy_capture_v1.c`,
`types/ext_image_capture_source_v1/output.c`, and the source interface. Its
`copy_buffer` helper and `wlr_texture.read_pixels` contract are synchronous;
`source.copy_frame`, however, can retain the protocol frame until a later
`ready` or `failed` event. The frame exposes a destroy signal for cancellation.

Vela now owns the output-source adapter in `scene/output_capture.c`, derived
from the MIT-licensed wlroots source with its cursor metadata/lifetime handling
preserved. Window capture uses the same completion helper in `scene/capture.c`.
No vtable patching, private wlroots struct casts, or early-success returns are
used. DMA-BUF destinations retain wlroots' existing GPU-to-GPU path. Legacy
`wlr-screencopy` and other texture consumers retain synchronous `read_pixels`.

For each SHM request:

1. Lock/import the source and lock the destination. Reserve a cached/coherent
   staging slice; record the same image ownership/layout and host-read barriers.
2. Submit on the renderer's queue and return from the source callback. These
   sources are compositor-owned output/window swapchains drawn on that same
   queue; output sessions disable direct scanout. A GPU timeline wait explicitly
   orders the readback after earlier rendering, and later renderer submissions
   wait for the copy before possibly reusing its source (including cancellation).
   Queue submission order alone provides no memory dependency. No CPU wait or
   sync-file-export fallback is needed. This API must not receive client buffers.
3. One renderer-wide timer checks the Vulkan timeline without waiting. After
   completion it copies at most 512 KiB or about 0.5 ms of rows per 1 ms tick,
   whichever comes first, serving requests round robin. A single row, page
   fault or OS scheduling delay can exceed the soft time budget; this is not a
   hard real-time guarantee. An idle callback would drain the entire queue in
   one dispatch and is deliberately not used.
4. Every destination mapping/access stays on the Wayland thread, including
   wl_shm SIGBUS protection. Stride padding and opaque-alpha conversion are
   preserved. No worker dereferences a client-owned mapping.
5. Send `ready` with the captured transform/time only after the final row.
   Failure, timeout, constraints changes, frame/session/client destruction,
   output removal and lock release reservations and retained references.

GPU completion alone cannot recycle staging: CPU readers pin its buffer by
handle, surviving pool array growth and compaction. Cancellation releases the
CPU pin; ordinary submission points still protect outstanding GPU work and
retired imported images. The renderer cancels remaining requests before teardown.

The queue accepts at most four requests and 128 MiB of pixel payload. Pool
allocation rounding and its existing bounded idle cache are additional. Excess
requests fail explicitly through the protocol; none report successful dropped
pixels. A failed timeline query/device loss or five-second timeout fails a
request rather than keeping its references indefinitely.

While a frame is pending, repeated output events do not queue duplicate work.
Output sources conservatively advertise full damage and request the next output
commit for each capture. This also prevents delayed `ready` from losing changes
that arrived while wlroots was accumulating damage for the previous capture.
It does not change scene rendering damage or presentation deadlines.

## Validation

`functional-capture` retains its original 2560x1440@180 animation workload,
minimum capture count and `missed < captures` assertion. Additional GPU cases
check full pixels, padded rows and opaque alpha across four simultaneous copies,
repeated capture on one unchanged session, explicit queue-limit failures,
frame destruction, disconnect, resize, unplug, lock and Vulkan-validated shutdown
with actual pending work. The private `state.readback` counters establish that
these tests really interrupted deferred requests and that payload returns to zero.

The temporary profiling/probe workflow steps are removed. Both manual workflows
run the ordinary full strict GPU suite with their original thresholds. Follow
[the GPU CI policy](ci-quality.md#gpu-ci-policy); a historical pass or a CPU-only
build does not establish that this implementation fixes NVIDIA performance.
