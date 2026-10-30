# Vulkan result handling and device-lost policy — design draft

> **Status:** implementation and hardware validation in progress. A sticky device-lost state and supervised exit are wired; final validation is still pending.

## Problem confirmed in current code

`compositor/src/render/renderer.c` does not recognize `VK_ERROR_DEVICE_LOST` as a distinct terminal state. Representative unchecked calls include `vkBindBufferMemory` and `vkMapMemory` in `new_staging_chunk()`, `vkCreateSemaphore` in `take_semaphore()`, `vkResetCommandBuffer`, `vkBeginCommandBuffer`, `vkEndCommandBuffer`, `vkWaitSemaphores`, and `vkDeviceWaitIdle`. Some creation paths check `VkResult`, but `vela_renderer_submit()` reduces `vkQueueSubmit2` to a boolean, losing the error category. Timeline progress may therefore be stale after a GPU reset, yet retirement/reuse proceeds as if the device were healthy. The session supervisor restarts on a compositor crash, but this is not an explicit device-loss recovery contract.

## Proposed policy

1. Introduce one checked-`VkResult` classification path with operation name and diagnostic context: success, recoverable/resource failure (context-specific), and terminal `VK_ERROR_DEVICE_LOST`. Never treat every Vulkan error as device lost.
2. Add a **sticky device-lost** state on the Vulkan/renderer owner. Once lost, stop submissions, waits, resource recycling and work that assumes timeline progress; report the first failing API and code.
3. Propagate failure through rendering/output calls to a single compositor-level shutdown decision. End the session's renderer/compositor predictably and let the existing supervisor handle bounded restart policy. Keep logging/reporting possible without calling invalid Vulkan operations.
4. Define cleanup **separately** for healthy initialization failure vs device loss. In particular do not blindly block on `vkDeviceWaitIdle`, `vkWaitSemaphores(..., UINT64_MAX)`, or timeline-value polling after loss; respect spec-allowed destroy/free paths.
5. Make per-operation ownership and failure handling explicit for mapped/bound staging buffers, semaphores, command buffers, imported dmabufs, exported sync files and output resources.
6. Plan a *separate future* design for re-creating a lost logical device within the same Wayland session; this is not required for beta 0.1.

## Fault-injection / test plan

- Wrap/abstract Vulkan result-returning operations at testable boundaries. Inject `VK_ERROR_DEVICE_LOST` at queue submit, timeline wait/query, memory map/bind and semaphore creation; assert no further submissions or invalid wait/reuse.
- Inject `VK_ERROR_OUT_OF_HOST_MEMORY`, `VK_ERROR_OUT_OF_DEVICE_MEMORY` and relevant initialization errors; assert precise paths rather than unconditional abort.
- Verify one diagnostic report, bounded supervisor behavior, proper fd/resource handling, and deterministic exit status; prevent busy restart loops.
- Run relevant AMD and NVIDIA GPU suites on the candidate revision plus a controlled real-device-reset/manual scenario when feasible.
- Distinguish **headless functional tests** from full DRM/KMS and suspend/resume coverage.

## Open design questions

How should the compositor communicate its terminal GPU fault to the supervisor? Which teardown calls must be suppressed after loss? Should controlled stop preserve lock/secure-session invariants when restarting? These require review before implementation.

## Implementation progress (30 October 2026)

Implemented in #147: central VkResult classifier and first-error logging; sticky device-lost state; termination of the compositor's Wayland event loop with exit code 79; existing supervisor's three-failures-per-minute restart policy, respecting the secure lock marker; no new command recording, GPU submission, timeline wait/query or GPU-resource recycling once lost. Healthy shutdown still waits and resets as before; device-loss teardown skips these operations. Asynchronous readbacks fail rather than copy incomplete pixels. Staging map/bind and import, semaphore, command-buffer, submission and readback return paths are checked. CPU-only policy and staging-bind fault tests exercise the state and gating.

Still required before declaring this beta blocker solved: a targeted queue-submit/wait fault-injection harness, reviewing remaining Vulkan result-returning paths, full AMD and NVIDIA GPU suites at the exact merge candidate, and a supervised locked-session smoke test. Never manufacture a real GPU reset on the daily-use workstation; hardware-reset tests require a disposable environment and explicitly controlled conditions.
