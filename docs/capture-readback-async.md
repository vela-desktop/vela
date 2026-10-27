# Non-blocking capture readback: implementation plan

> **Status:** implementation draft, not a shipped fix. This document records the
> diagnostic handoff from [PR #155](https://github.com/vela-desktop/vela/pull/155).
> No change to screenshot correctness, protocol readiness, or frame budgets is
> authorized merely by merging this design. The PR that introduces working code
> must pass real hardware tests first.

## 1. Evidence and measurable problem

The NVIDIA RTX 4070 Laptop (proprietary Vulkan driver 615.78.08) misses
presentation deadlines while taking repeated screenshots during animation.
An otherwise identical AMD RADV runner passes its 20 GPU tests.

| NVIDIA workload | Captures | Missed vblanks | Readback p50 | CPU memcpy p50 | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| 2560x1440 @ 180 Hz | 40 | 81 | 12.186 ms | 10.694 ms | fail |
| 1920x1080 @ 180 Hz | 43 | 45 | 8.068 ms | 6.995 ms | fail |
| 2560x1440 @ 60 Hz | 37 | 0 | 11.931 ms | 10.340 ms | pass |

Source: [NVIDIA A/B run #41](https://github.com/vela-desktop/vela/actions/runs/40586655037)
on commit `2f2f7df`; [matching AMD run #43](https://github.com/vela-desktop/vela/actions/runs/40586663325)
passed 20/20. The normal NVIDIA full suite failed only `functional-capture`
(19/20 passed). These are elapsed CPU times, not Vulkan GPU timestamps.

The 180 Hz frame interval is about 5.56 ms; 60 Hz permits about 16.67 ms.
Reducing the pixel count helps but does not make a synchronous full-frame
readback fit into the 180 Hz budget. The staging allocation reports both
`HOST_CACHED` and `HOST_COHERENT`; a memory-type flag alone has not solved it.

## 2. Current synchronous boundary

The current `wlr_texture_impl.read_pixels` implementation in
`compositor/src/render/texture.c`:

1. Allocates/reuses a host-visible readback staging slice via
   `vela_renderer_stage_readback()`.
2. Records image ownership/layout transitions and `vkCmdCopyImageToBuffer`,
   including a device-to-host buffer barrier.
3. Submits the copy and registers dmabuf synchronization where applicable.
4. **Blocks** on `vela_renderer_wait(renderer, point)`.
5. **Copies** mapped staging bytes row-by-row to the destination, potentially
   fixing up alpha, before returning to the caller.

`compositor/src/scene/capture.c` invokes the wlroots
`wlr_ext_image_copy_capture_frame_v1_copy_buffer()` path and currently emits
`ready` only after that call succeeds. Do **not** replace the blocking wait
with a deferred worker in `read_pixels()` while returning success immediately:
the caller's destination may be invalidated or recycled and `ready` could
be signaled before valid pixels exist.

Other output capture and wlroots renderer consumers may invoke this synchronous
texture method too. Enumerate them before choosing where the async boundary
belongs. The correct fix may require an asynchronous Vela-owned copy/capture
path, not merely a different Vulkan staging allocator.

## 3. Desired completion model (pending code review of wlroots call sites)

- Frame production schedules a readback request without waiting for GPU copy
  completion or performing large CPU copies on the presentation thread.
- An in-flight request *owns* all resources that outlive the initiating
  callback: destination buffer lock/reference, staging reservation, imported
  image/buffer lifetime, relevant timeline point and any synchronization FDs.
- GPU readiness is detected without busy-polling the Wayland event loop
  (fence/timeline integration or bounded completion worker). The CPU copy
  happens away from the critical presentation path where threading rules
  permit; otherwise split the work at a safe asynchronous protocol boundary.
- Completion transitions back to the correct Wayland/event-loop context,
  publishes readiness **only after** the output buffer contains the correct
  pixels, and releases references and staging reservations in order.
- Concurrent captures use a bounded in-flight queue/ring. Backpressure never
  silently overwrites or reuses staging data still referenced by GPU/CPU work.
  Treat cancellation, resize, output removal, session disconnect, shutdown and
  device loss as first-class terminal states.
- Preserve foreign dmabuf ownership transfers, imported/exported sync files,
  image layout restoration, host visibility/invalidation semantics, modifier
  handling, row stride and alpha fixup. Verify synchronization rules for both
  coherent and non-coherent allocations.
- If a synchronous wlroots API is unavoidable for a subset of callers, keep a
  correct fallback. Do not silently claim those paths have become asynchronous.

The implementation should expose a single request state machine and a
well-defined cancellation/retirement contract. Avoid adding an indefinitely
growing worker pool, unbounded mapped allocations or per-frame timers.

## 4. Implementation checkpoints

### A. Identify the asynchronous seam

Map every call that reaches `wlr_texture_impl.read_pixels`, including
ext-image-copy-capture, screencopy and internal snapshots. Determine where
buffers can be retained and where a protocol frame can be completed later.
Document whether upstream wlroots APIs permit deferred completion or require
a Vela-owned copy path. **This is a gate before changing the renderer.**

### B. Build a minimal vertically integrated path

Implement one capture flow end to end: submit Vulkan copy; track a completion
point and resources; finish the CPU copy and only then send frame readiness.
Preserve the existing path for callers not migrated. Establish bounded queue
capacity and a safe cancellation/drain protocol before adding concurrency.

### C. Extend coverage and observability

Keep diagnostic phases from #155 available to this PR as needed, without
forcing profiling on every GPU CI run. Track CPU time spent inside the
presentation callback separately from total capture completion latency.
Account for dropped/cancelled captures, in-flight count and staging reuse.
Use the existing strict `functional-capture` oracle: don't weaken or skip it.

### D. Verify both vendors and lifecycle behavior

- NVIDIA 2560x1440@180: `functional-capture` passes with
  `missed_vblanks < captures`; aim for zero misses without claiming a fixed
  performance promise beyond the test oracle.
- AMD RADV: all 20 GPU tests continue to pass, including image sharpness.
- 2560x1440@60 and 1920x1080@180: no regressions in image contents or
  performance; compare against #155 A/B measurements.
- Check pixel equality, stride handling, XRGB-to-ARGB opaque alpha, foreign
  dmabuf transfer, concurrent screenshots, compositor teardown with pending
  work, resizing/invalidating source and destination buffers, error/cancel
  paths and bounded resource retention.
- Validate release-mode and debug builds; run normal CI and manually dispatch
  the real hardware NVIDIA/AMD suites. Ordinary host CI passing is not
  evidence of GPU performance correctness.

## 5. Out of scope for this fix

No refresh-rate caps, longer render-frame deadlines, dropped presentation
checks, relaxed `functional-capture` assertions, unconditional `vkQueueWaitIdle`,
backend-specific test skips, or speculative memory-type switches.

## 6. Relationship to PR #155

[PR #155](https://github.com/vela-desktop/vela/pull/155) is a completed
diagnostic experiment; its branch-scoped A/B workflow and verbose profiling
are not intended as permanent mainline production behavior. Reuse the
instrumentation selectively as needed to prove that this fix works. The new
branch is based on `main` so the implementation does not implicitly inherit
temporary CI workload or source-level profiler overhead.
