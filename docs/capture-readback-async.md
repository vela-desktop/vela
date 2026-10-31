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
| [#156](https://github.com/vela-desktop/vela/pull/156) | Four CPU copy workers shortened a synchronous stall, but NVIDIA still failed. The workers were removed; the PR was merged with deferred completion instead. |

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
3. One renderer-wide one-shot `timerfd` checks the Vulkan timeline without
   waiting. After completion it copies about 0.5 ms of rows, serving requests
   round robin. Each slice returns through `epoll`, then yields for 50 µs;
   when no job is GPU-ready it polls after 200 µs. A single row, page fault or
   OS scheduling delay can exceed the soft budget; this is not a hard real-time
   guarantee. An idle callback would drain the entire queue in one dispatch
   and is deliberately not used. There is no fixed byte allowance per tick.
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

Consumers of the same locked compositor swapchain image share its GPU-to-host
copy and staging bytes. Each destination retains its own CPU progress, texture
and source-buffer references, and staging pin. Cancelling one consumer cannot
release storage still used by another. Sharing ends with the last pending
consumer: this is not a pointer-keyed cache for mutable client buffers. The
private `readback.submitted` counter counts actual deferred GPU copies; the
four-success overflow case requires one GPU copy of the shared source image.

The queue accepts at most four requests and 128 MiB of pixel payload. The entire
readback staging pool also has a 128 MiB allocation limit, including Vulkan
alignment, idle cached chunks and cancelled copies still in flight on the GPU.
Pressure evicts only idle chunks with no CPU reader and a completed timeline
point; active readers and cancelled GPU work retain their reservation. Excess
requests fail explicitly through the protocol; none report successful dropped
pixels. A failed timeline query/device loss or five-second timeout fails a
request rather than keeping its references indefinitely.

While a frame is pending, repeated output events do not queue duplicate work.
The output adapter keeps damage from later commits outside wlroots until every
frame using the offered image has terminated: wlroots' `ready` clears its
session damage, so feeding newer damage before that point would lose the final
change if the desktop then stopped updating. The next source event carries the
accumulated real damage, including when the newly requested output commit has
no further scene changes. A subsequent capture on an unchanged session waits
for source damage, as allowed by the protocol; it does not continuously wake
presentation or advertise full-screen damage.

Output sources are shared per Wayland client and output, and are destroyed on
client disconnect or output removal. This confines the damage frontier to that
client's sessions: a slow SHM capture does not hold back another client's
DMA-BUF capture. Sessions sharing one source still advance in batches, at the
pace of their slowest readback. The renderer's queue and CPU budget remain
global. No wlroots private session fields are accessed or patched.

## Why the first green CI result was insufficient

The first deferred candidate used 512 KiB / 0.5 ms slices with a 1 ms timer.
The byte allowance alone required at least 28 copy ticks at 1440p and 62 at 4K
for BGRA8. Multiple sessions shared those ticks. Passing the animation test
therefore established presentation responsiveness, without establishing
acceptable sustained capture throughput or latency. Its client also sleeps
50 ms between screenshots and creates a fresh session each time.

The persistent-stream probe measures request-to-ready p50/p95/p99/max and
completed frames per second over three seconds without capture-side sleeps or
PNG encoding. It compares untouched new SHM destinations with buffer reuse at
1440p and 4K, plus four simultaneous sessions on one connection at 1440p.
Maximize/restore animations provide the source changes. Rates for four sessions
are aggregate; per-session frame counts detect starvation. These are measured
workloads rather than claims of maximum capture rate on all desktop content.
The idle case settles the window first, then requires one initial capture per
session and no more than two output frames over three seconds.

For cached staging, new SHM buffers must exceed 40 fps at 1440p / 20 fps at 4K,
beyond the old byte-paced scheduler's theoretical ceiling. Reused buffers must
sustain 60 / 30 fps respectively. Request-to-ready p95 must remain below 100 ms
for one session and 150 ms for four. The original uncached-staging exception is
preserved; measurements and pixel/lifetime/idle checks still run there.
The original `missed < captures` presentation budget also applies to these
sustained streams, so capture throughput alone cannot satisfy the new checks.
In addition, no more than 5% of observed presentation slots may be missed
(`missed / (output_frames + missed)`). This bound is independent of the number
of capture sessions: four sessions' aggregate rate cannot mask a slower desktop.
Sharing the GPU copy avoids transferring an identical image once per consumer.

## Validation

`functional-capture` retains its original 2560x1440@180 animation workload,
minimum capture count and `missed < captures` assertion. Additional GPU cases
check full pixels, padded rows and opaque alpha across four simultaneous copies,
repeated capture on one session with controlled source changes, idle waiting,
persistent-stream latency and throughput, explicit queue-limit failures,
frame destruction, disconnect, resize, unplug, lock and Vulkan-validated shutdown
with actual pending work. The private `state.readback` counters establish that
these tests really interrupted deferred requests and that payload returns to zero.

CPU-only `compositor.readback` tests use the production helpers with Vulkan
entry points stubbed to force binding, mapping and command allocation failures.
They check cleanup before memory release, recycling an unsubmitted reservation,
unaligned/padded rows and opaque alpha in 8-bit, packed 10-bit and half-float
pixels without changing RGB. The GPU color oracle uses asymmetric `#123456`
pixels; queue-overflow checks require four capacity-specific `rejected` events,
so unrelated allocation or timeout failures cannot stand in for a full queue.
The CPU tests also simulate GPU lag after cancellation, completion and idle-cache
eviction, proving the allocation limit cannot be bypassed by cancelling frames.
The pixel fixture waits for the wallpaper's first captured color, rather than
assuming that a fixed startup delay is sufficient.

The GPU-free `compositor.capture-damage` test exercises the production output
adapter's emitted regions and output scheduling. Two pending frames finish at
different times; later damage must survive both terminal notifications and be
offered even if no further desktop change occurs. Idle requests must not wake
the output, and teardown must not schedule another frame. GPU lifetime tests
sample pending work every millisecond and use four concurrent 4K copies so that
faster completion does not make a 50 ms observation interval miss the operation.

The temporary profiling/probe workflow steps are removed. Both manual workflows
run the ordinary full strict GPU suite with their original thresholds. Follow
[the GPU CI policy](ci-quality.md#gpu-ci-policy); a historical pass or a CPU-only
build does not establish that this implementation fixes NVIDIA performance.
