# Shell graphics memory

> **Status:** Accepted. **Checked:** 24 November 2026, `66bdff57` and this change.
>
> **Remaining:** the taskbar's and wallpapers' allocators (about 176 MiB at idle on one output, §2) need a block size Qt doesn't let the shell choose.

Since #286 the shell renders Qt Quick with Vulkan. On one 1920×1080 output
it held about 200 MiB of VRAM at idle. **Every panel opened once held about
80 MiB of VRAM and 32 MiB of GTT more, until the shell exited.** Opening and
closing Start, Notification Center, Quick Settings, the clipboard history and
Task View took the shell from 202.7 to 597.8 MiB of VRAM. Hidden panels now
give their graphics back 10 seconds after they close.

## 1. Measurements

Radeon RX 9070 XT (RADV, Mesa 26.3.1), Qt 6.12.0, Linux 7.3.6, headless
output at 1920×1080 and 100 Hz, `QSG_RENDER_LOOP=threaded` as in a session.
VRAM and GTT are the `drm-resident-vram` and `drm-resident-gtt` that the
shell's DRM client reports in `/proc/PID/fdinfo`. Open times run from the
shell command to the compositor's map of the layer surface, after a
`zwlr_layer_surface_v1` configure handshake.
[`benchmarks/shell-graphics-memory.py`](benchmarks/shell-graphics-memory.py)
takes one sample. Three samples per build, alternating, are in
[`shell-graphics-memory-2026-11-24.csv`](benchmarks/shell-graphics-memory-2026-11-24.csv).
The values below are their medians.

| | `main` | Release after 10 s |
| --- | ---: | ---: |
| VRAM / GTT at idle | 202.7 / 66.0 MiB | 202.7 / 66.0 MiB |
| 1 s after closing the five panels | 597.8 / 226.1 MiB | 597.8 / 226.1 MiB |
| 11 s after closing them | 598.8 / 226.1 MiB | **202.8 / 66.0 MiB** |
| A panel's first opening | 10.5 ms | 10.5 ms |
| Reopened within 10 s | 4.4 ms | 4.4 ms |
| Reopened after 10 s | 4.4 ms | **8.6 ms** |
| Start's frames while it opens, after 10 s (median / max gap) | 10.0 / 10.3 ms | 10.0 / 10.2 ms |

## 2. Where the memory goes

With the threaded render loop, Qt Quick gives every `QQuickWindow` its own
`QRhi`, with its own Vulkan device and its own allocator (VMA). VMA reserves
memory in blocks of 32 MiB per memory type. It keeps one empty block of each
type when its allocations go. The shell's windows need far less than that.
A temporary build printed `QRhi::statistics()` after every frame:

| Window | Allocated | Reserved but unused |
| --- | ---: | ---: |
| Taskbar | 4.1 MiB | 92 MiB |
| Wallpaper | 11.3 MiB | 84.7 MiB |
| Notification Center | 0.5–2.2 MiB | 65–96 MiB |
| Start | 0.8 MiB | 97.5 MiB |

At idle, the taskbar's and the wallpaper's unused blocks are about 176 MiB.
The taskbar and the wallpaper are per output, so a second monitor adds
roughly as much again. Since #5, a hidden panel destroys its native surface
and swapchain (`releaseHiddenPanel`). Its `QRhi` stayed, because
`QQuickWindow::persistentGraphics` defaults to true. The shell has 21 kinds
of panel, tooltips and taskbar previews among them. Every one of them that
had been shown kept its own blocks.

## 3. Alternatives

- **Release on closing.** `persistentGraphics` and `persistentSceneGraph`
  false: the memory comes back at once, but every opening pays a new device,
  8.4 ms instead of 4.3 ms. Rejected.
- **One `QRhi` for every window** (`QQuickGraphicsDevice::fromRhi`). One
  allocator: 137.4 MiB at idle and 139.5 MiB after four panels. Qt accepts a
  shared `QRhi` only with the `basic` render loop, which animates every window
  on one timer: with a single window animating its frames came every 16 ms
  instead of every 10 ms. Rejected: animations follow the output's refresh
  rate.
- **Release the scene graph only**, keeping the `QRhi`: the empty blocks stay
  with VMA. 608.7 MiB after the five panels. Rejected.
- **Smaller blocks.** VMA can be told a block size, but `QRhiVulkan` creates
  its allocator without exposing that setting. This would need a change in Qt.

## 4. The change

`releaseHiddenPanel` starts a 10-second timer when a panel hides. Shown
again, the panel stops the timer and keeps its `QRhi`. When the timer fires,
the panel sets `persistentGraphics` and `persistentSceneGraph` to false and
calls `QQuickWindow::releaseResources()`. The next time it is shown, it sets
both back to true. Otherwise the next `destroy()` on hide would release them
at once. The taskbar and the wallpapers stay mapped and are not affected.

Costs, measured with a temporary build that timed each step:

- **Reopened after 10 s,** a panel opens as on its first opening. Its first
  frame comes about 4 ms later: on the render thread the frame ends about
  1.5 ms later, then the first submission of a new Vulkan device keeps the
  GPU about 4 ms longer before Mesa attaches the buffer. Within 10 s nothing
  changes.
- **The release** blocks the GUI thread for 0.8–1.8 ms while the render
  thread destroys the `QRhi`. The panel is already hidden at that point.
  When that release came during Start's opening animation, no frame was
  late. The longest gap between Start's frames was 10.9 ms at 100 Hz.
- **The rebuilt panel** looks the same. Its text can differ by one or two
  levels of antialiasing, because the glyph cache is filled in another order.

The functional test `PanelGraphics` covers each trigger:

- reopened within 10 s, the panel keeps its graphics while open;
- after closing, it releases them 10 s later, not sooner;
- where the driver reports VRAM, the shell is back to its idle VRAM and GTT;
- reopened, the panel looks the same, and it releases again 10 s after the
  next close.

Removing the release, the restored flags or the stopped timer each makes
the test fail.
