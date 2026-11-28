# Shell graphics memory

> **Status:** Accepted. **Checked:** 24 November 2026, `66bdff57` and this change.
>
> **Remaining:** four changes in Qt (§6): a command buffer leak on every destroyed swapchain, VMA's block size, which would take the shell from 212.6 to about 102 MiB of VRAM at idle, remembering a per-font check that slows a panel's first frame, and curve-renderer Shapes left empty on a new scene graph, which the shell works around.

Since #286 the shell renders Qt Quick with Vulkan. On one 1920×1080 output
it held about 200 MiB of VRAM at idle. **Every panel opened once held about
80 MiB of VRAM and 32 MiB of GTT more, until the shell exited.** Opening and
closing Start, Notification Center, Quick Settings, the clipboard history and
Task View took the shell from 202.7 to 597.8 MiB of VRAM.

A hidden panel now keeps its surface, swapchain and graphics for 10 seconds,
then gives them all back. Reopened within 10 s, it opens in 2.3 ms instead of
4.4 ms. After 10 s it opens as it did the first time, in about 8.7 ms.

## 1. Measurements

Radeon RX 9070 XT (RADV, Mesa 26.3.1), Qt 6.12.0, Linux 7.3.6, headless
output at 1920×1080 and 100 Hz, `QSG_RENDER_LOOP=threaded` as in a session.
VRAM and GTT are the `drm-resident-vram` and `drm-resident-gtt` that the
shell's DRM client reports in `/proc/PID/fdinfo`. Open times run from the
shell command to the compositor's map of the layer surface. The shell's
first frame comes after a `zwlr_layer_surface_v1` configure handshake.
[`benchmarks/shell-graphics-memory.py`](benchmarks/shell-graphics-memory.py)
takes one sample. Three samples per build, alternating, are in
[`shell-graphics-memory-2026-11-24.csv`](benchmarks/shell-graphics-memory-2026-11-24.csv).
The values below are their medians.

| | `main` | This change |
| --- | ---: | ---: |
| VRAM / GTT at idle | 202.7 / 66.0 MiB | 202.7 / 66.0 MiB |
| 1 s after closing the five panels | 597.8 / 226.1 MiB | 650.5 / 226.1 MiB |
| 11 s after closing them | 598.8 / 226.1 MiB | **202.8 / 66.0 MiB** |
| A panel's first opening | 10.5 ms | 10.5 ms |
| Reopened within 10 s | 4.4 ms | **2.3 ms** |
| Reopened after 10 s | 4.4 ms | 8.7 ms |
| Start's frames while it opens, after 10 s (median / max gap) | 10.0 / 10.1 ms | 10.0 / 10.2 ms |

Within the 10 seconds the five swapchains add 52.7 MiB. They used to be
destroyed on closing.

## 2. Where the memory goes

With the threaded render loop, Qt Quick gives every `QQuickWindow` its own
`QRhi`, with its own Vulkan device and its own allocator (VMA). VMA reserves
memory in blocks of 32 MiB per memory type, and keeps one empty block of
each type when its allocations go. The shell's windows need far less than
that. A temporary build printed `QRhi::statistics()` after every frame:

| Window | Allocated | Reserved but unused |
| --- | ---: | ---: |
| Taskbar | 4.1 MiB | 92 MiB |
| Wallpaper | 11.3 MiB | 84.7 MiB |
| Notification Center | 0.5–2.2 MiB | 65–96 MiB |
| Start | 0.8 MiB | 97.5 MiB |

At idle, the taskbar's and the wallpaper's unused blocks are about 176 MiB.
The taskbar and the wallpaper are per output, so a second monitor adds
roughly as much again.

Since #5, a hidden panel destroyed its native surface and swapchain. Its
`QRhi` stayed, because `QQuickWindow::persistentGraphics` defaults to true.
The shell has 21 kinds of panel, tooltips and taskbar previews among them.
Every one of them that had been shown kept its own blocks.

## 3. Where the time goes

The same temporary build marked Qt Quick's render-thread signals on the
clock of the `WAYLAND_DEBUG` trace. Opening Start:

| Step | Reopened, surface kept | Reopened, surface destroyed (`main`) | After the release |
| --- | ---: | ---: | ---: |
| New layer role, configure and ACK | 0.3 ms | 0.3 ms | 0.3 ms |
| New swapchain: Mesa's surface queries, one Wayland round trip each | none | ~1.7 ms | ~1.7 ms |
| New Vulkan device and `QRhi` | none | none | ~1.5 ms |
| Synchronizing the scene graph | <0.1 ms | <0.1 ms | 3.2 ms |
| Rendering the first frame | <0.1 ms | <0.1 ms | 1.1 ms |

The GPU finishes that first frame about 40 µs after Qt submits it, even
after the release. Synchronizing blocks the GUI thread. After the release,
`QSGThreadedRenderLoop` reported it blocked for 8 ms.

A sampling profiler on the render thread, active only while the first sync
after a release ran (2461 samples, 12 openings of Start), found 92% of that
time in `QQuickText::updatePaintNode`:

- 41%: `qt_fontHasNarrowOutlines`. Each new distance-field glyph cache
  rasterizes an "O" at 54 px, with FreeType's TrueType interpreter, to choose
  its scale. The answer depends only on the font.
- 26%: glyph outlines extracted for new distance fields.
- 20%: font engines loaded again through fontconfig. Qt's font cache is per
  thread, and the profile suggests the render thread is a new one.

Without hinting (a fontconfig override) the sync took 3.0 instead of 3.3 ms.
That is not worth the change in how text looks.

## 4. Alternatives

- **Release on closing.** The memory comes back at once, but every opening
  pays a new device and a new scene graph: 8.4 ms. Rejected.
- **One `QRhi` for every window** (`QQuickGraphicsDevice::fromRhi`). One
  allocator: 137.4 MiB at idle and 139.5 MiB after four panels. Qt accepts a
  shared `QRhi` only with the `basic` render loop, which animates every window
  on one timer. With a single window animating, its frames came every 16 ms
  instead of every 10 ms. Rejected: animations follow the output's refresh
  rate.
- **Release the scene graph only**, keeping the `QRhi`: the empty blocks stay
  with VMA. 608.7 MiB after the five panels. Rejected. The other way round
  isn't possible: releasing the graphics releases the scene graph too.
- **Prepare a panel before it opens**, for example when Super is pressed. A
  layer-shell window gets its `QRhi` only once mapped, so the panel would have
  to be mapped while invisible. That risks taking the keyboard focus and
  covers neither the mouse nor the shell socket. Rejected.
- **One window for all the panels of a layer.** It would share one `QRhi`,
  but the window covers the output: at 3840×2160 its four swapchain images
  alone are 126 MiB, and Qt Quick redraws the whole window on every frame.
  Rejected.

## 5. The change

`releaseHiddenPanel` starts a 10-second timer when a panel hides:

- **Hidden,** the window keeps its `wl_surface`, swapchain and `QRhi`.
  LayerShellQt destroys the layer role and detaches the buffer.
- **Shown again within 10 s,** the timer stops and the surface gets a new
  layer role, configured before its first buffer. The blur object stays bound
  to the surface and receives its region again. On another display (Start
  from the second taskbar, or a panel opened from the keyboard on the display
  in use), Qt recreates the window for its new screen: the panel opens on a
  new surface and the old one goes, while the `QRhi` stays.
- **When the timer fires,** the panel calls `destroy()` for the surface and
  swapchain, sets `persistentGraphics` and `persistentSceneGraph` to false,
  and calls `QQuickWindow::releaseResources()`. Shown again later, it sets
  both back to true, and has its curve-renderer Shapes build their paths
  again, which Qt leaves empty on a new scene graph (§6, #326).

The taskbar and the wallpapers stay mapped and are not affected.

#5 destroyed the surface on every hide because, with EGL, Qt attached the
previous buffers to the new layer role before its configure. With Vulkan the
whole `test_shell` suite passes with the surface kept.

Costs:

- **Reopened after 10 s,** a panel opens as on its first opening, about 4 ms
  later than `main` (§3).
- **The release** blocks the GUI thread for 0.8–1.8 ms while the render
  thread destroys the `QRhi`. The panel is already hidden at that point. When
  that release came during Start's opening animation, no frame was late: the
  longest gap between Start's frames was 10.9 ms at 100 Hz.
- **The rebuilt panel** looks the same. Its text can differ by one or two
  levels of antialiasing, because the glyph cache is filled in another order.
- **Finding the curve-renderer Shapes** of a panel shown again after the
  release walks its items on the GUI thread: 12–110 µs (Start about 90,
  Quick Settings the most). In three alternating samples with and without
  it, every panel reopened after 10 s in 8.3–10.5 ms either way, and Start's
  frames while it opened kept a 10.0 ms median.

The functional tests cover each trigger:

- `PanelGraphics`:
  - reopened within 10 s, the panel keeps its surface and graphics while
    open;
  - after closing, it gives them back 10 s later, not sooner;
  - where the driver reports VRAM, the shell is back to its idle VRAM and GTT;
  - reopened after the release, the panel opens on a new surface, looks the
    same, and releases again 10 s after the next close;
  - reopened after the release, Start still draws its power symbol and,
    in the power menu, Sign out's: Shapes on the curve renderer.
- `test_reopened_menu_keeps_its_surface_and_blur` and
  `test_other_panels_reopen_on_their_kept_surface`: a new layer role on the
  same surface, configured before its buffer, with Start's blur.
- `test_a_kept_panel_reopens_on_the_display_in_use`: Start and Quick
  Settings, kept, open on the display in use; on the same display on their
  kept surface, on another one on a new surface, the old one destroyed.

Each of six changes makes these tests fail: destroying the surface on hide,
not destroying it at the release, not releasing, not restoring the flags,
not stopping the timer, and not rebuilding the curve-renderer Shapes.

## 6. Changes in Qt

The first three were measured with Qt 6.12.0's QtGui rebuilt with the change,
loaded by the shell through `LD_LIBRARY_PATH`.

- **A leak on every destroyed swapchain.** `QVkSwapChain::destroy()` forgets
  its two primary command buffers (`frame.cmdBuf = VK_NULL_HANDLE`) without
  `vkFreeCommandBuffers`. They stay in the `QRhi`'s command pools with the
  memory RADV gave them, about 190 KiB of VRAM per swapchain. On `main`, which
  destroyed a panel's surface on every hide, each opening of any panel added
  that much: 60 openings of Start took the shell from 289.9 to 301.2 MiB.
  Freeing them from `cmdPool[i]` kept it at 290.0 MiB. A Vulkan program
  without Qt that creates and destroys 100 Wayland swapchains on the same
  device doesn't grow, so Mesa and RADV are not involved.

  With this change, a panel reopened on the same display keeps its
  swapchain, and destroys it with its `QRhi`, whose pools go too: 60 quick
  openings of each panel kept the shell flat. A panel that opens on another
  display within 10 s still loses a swapchain with its `QRhi` alive, and holds
  those 190 KiB until the release: 40 openings of Start alternating between
  two displays added 7.3 MiB, and 10 s after the last one the shell was back
  at its idle VRAM and GTT (+12 KiB).

- **VMA's block size.** `QRhiVulkan` creates its allocator without
  `preferredLargeHeapBlockSize`, so VMA uses its own default of 256 MiB, whose
  first blocks are 32 MiB. Flutter's Impeller sets 4 MiB for the same
  reason. With 4 MiB:

  | | Block size unset (today) | 4 MiB |
  | --- | ---: | ---: |
  | VRAM / GTT at idle | 212.6 / 66.0 MiB | 102.1 / 3.0 MiB |
  | After four panels | 521.9 / 194.0 MiB | 167.3 / 5.0 MiB |

  First openings took as long. With 1–2 MiB the shell saved a little more;
  with 16 and 64 MiB it held 129.1 and 164.6 MiB at idle.

- **`qt_fontHasNarrowOutlines` remembered per font.** The first sync after a
  release took 1.86 ms instead of 3.23 ms for Start, and 1.58 ms instead of
  2.80 ms for Notification Center. The first frame came 1.1–1.3 ms sooner.

- **Curve-renderer Shapes on a new scene graph** (#326). Reopened after the
  release, Start lost its power symbol and Sign out's, the only Shapes there
  with `preferredRendererType: Shape.CurveRenderer`, while the button still
  worked. On the new scene graph `QQuickShape::updatePaintNode()` creates a
  new root node, and `QQuickShapeCurveRenderer::setRootNode()` forgets the
  old nodes without marking the paths dirty, so `updateNode()` adds nothing.
  `QQuickShapeGenericRenderer::setRootNode()` sets `DirtyList` and rebuilds.
  The code is the same in Qt 6.11.2, 6.12.1 and `dev`. Marking the paths
  dirty in `setRootNode()` would come too late: it runs on the render thread
  after that frame's `sync()`. The Shape has to mark them before its next
  polish when its nodes go.

  Until then the shell does it itself: shown again after a release, a panel
  sets `preferredRendererType` of each curve-renderer Shape to the geometry
  renderer and back. The setter marks every path `DirtyAll`; the renderer is
  chosen only in `updatePolish()`, so it stays the same and the first frame
  computes the paths once, as on the panel's first opening.
