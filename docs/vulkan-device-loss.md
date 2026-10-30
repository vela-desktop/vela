# Vulkan device-loss and fatal-render policy — beta 0.1

> **Status (30 October 2026):** implemented on PR #147; hosted CI and real
> supervised locked-session / AMD / NVIDIA hardware validation remain merge gates.

## Failure classification

- `VK_ERROR_DEVICE_LOST` latches `vk->loss.lost` on the first error and
  records the originating operation. It is terminal, distinct from
  `VK_ERROR_OUT_OF_HOST_MEMORY`, `VK_ERROR_OUT_OF_DEVICE_MEMORY` and other
  recoverable errors.
- An unrecoverable render/synchronization failure (for example a failed GPU
  timeline wait after a successful queue submission) sets
  `vk->terminal_error`, **not** `vk->loss.lost`. The compositor must never
  signal a client-facing completion point when GPU completion is unknown.
- Both terminal states terminate the Wayland event loop and prevent additional
  GPU work, pool reuse, timeline polling and blocking waits.
- `VELA_EXIT_DEVICE_LOST = 79` is reserved for true device loss, including
  failure during renderer initialization. Other terminal failures exit with
  code 1. The existing supervisor uses its bounded crash budget (three failures
  within one minute); a clean logout still exits with 0.
- After terminal failure, skip the compositor's systemd `stop` hook: the
  supervisor keeps the session and Wayland socket across a restart and invokes
  the stop hook if it finally ends the session.

## Resource and synchronization ownership

The renderer checks Vulkan results for queue submission, command buffer
allocation/reset/begin/end, timeline wait/query, semaphore creation/import/
export, DMA-BUF image import and staging allocation/bind/map. A failure while
ending a command buffer frees command-slot reservations and unsubmitted staging
claims, allowing a future frame to retry if the device is still healthy.

A device loss discovered while lazily creating a drawing or blur pipeline stops
command recording immediately; commands are **not submitted**. Staging uploads
must not record commands into a null command buffer.

For a healthy shutdown, wait for the device and discard pending command
recordings before freeing resources. For device loss or a fatal render/sync
error, do not wait on the device or recycle GPU resources using stale timeline
values. Vulkan destruction/freeing and process exit are still possible.
Capture never copies unready pixels; asynchronous readbacks fail.

A failed kernel sync-file import may fall back to a CPU GPU-timeline wait.
When that wait fails, do not claim that the buffer has been released. Trigger
the terminal-error restart path instead. When a synchronization point can be
safely completed by waiting, signal it only **after** successful completion.

## Testing and merge gates

- CPU-only device-loss policy: first failing operation is sticky; ordinary
  allocation errors are not classified as device loss.
- Production-entry-point fault injection: staging bind/map, semaphore
  creation, queue submission, timeline wait, non-device-loss wait error and
  failed command-buffer end. Assert no GPU work after a terminal state.
- Add tests for further Vulkan import/export and lazy pipeline failures where
  practical, using the GPU-free production harness.
- Hosted Build, Ubuntu 26.10, Fedora, sanitizer, coverage and static-analysis
  checks must pass on the candidate head; investigate SonarQube reliability
  findings rather than treating the aggregate rating as sufficient.
- Run AMD and NVIDIA full GPU-labeled suites at the **exact merge-candidate
  SHA**. Test a supervised locked-session compositor restart in a disposable
  environment; verify the secure lock marker and session environment survive
  until the supervisor decides to end the session.
- Never trigger a real GPU reset on the daily-use workstation. Real driver
  reset tests require disposable hardware and an explicit controlled plan.

## Scope

Beta 0.1 does **not** reconstruct the Vulkan logical device in-place. A future
design may consider it independently; session-level supervised restart is the
intended recovery boundary.
