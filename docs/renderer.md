# The Vela renderer

> **Design history:** the stage log and risk register below preserve decisions
> made earlier in development. Descriptions such as “postponed” refer to the
> historical stage, not necessarily the current build. See the [README](../README.md),
> [hardware coverage](hardware.md) and [testing guide](testing.md) for current
> documented support and validation limits.

Design of Vela's own scene and renderer, native to Vulkan. It is the foundation
of milestone 2 (blur, rounded corners, shadows) and of everything that comes
after. This document comes before the code: decisions are made here, and the
code follows them.

> **Status:** design discussed and approved in its fundamental choices (§2,
> also summarized in §13). Stages **S0–S6 are done** (§11): Vela's scene and
> renderer are the only ones, apps reach the screen bit for bit at every scale,
> each frame is drawn as late as possible before the vblank, windows have
> rounded corners and shadows, the shell's panels have acrylic blur, Qt/KDE
> apps have Vela's title bar, and an app whose GPU is late never makes the
> screen wait (§7.3). Next: S7.

## 1. Goals

On any PC, Vela must be the most fluid and the sharpest desktop around. For the
renderer this means, in order of importance:

1. **Perfect sharpness at any scale.** At 100%, 125%, 150%, 175% and 200%, the
   text and lines of a well-written app must reach the screen **pixel for
   pixel**, with no resampling at all. No blur from scaling, no one-pixel line
   turning into two gray lines, no "shimmering" of windows while they move.
2. **Fluidity at any refresh rate.** 60, 75, 120, 144, 165, 180, 240 and 360 Hz,
   and VRR. Animations must take the same time on every monitor and use every
   frame the monitor offers. No constant tuned for one refresh rate.
3. **Minimal latency.** As little as possible from a mouse movement or a key
   press to the pixel on screen: draw as late as possible before the vblank,
   not as soon as the previous vblank arrives.
4. **Lightness.** Only what changes is drawn; with no changes, the GPU sleeps.
   Effects cost in proportion to how much of them is visible.
5. **Windows 11 effects and beyond:** live blur, rounded corners, shadows, all
   antialiased and sharp at every scale.

## 2. Decisions taken

| Decision | Choice | Consequence |
|---|---|---|
| Scene | **entirely our own**, not `wlr_scene` | what `wlr_scene` did (damage, feedback, scanout...) we do ourselves, and do better (§5) |
| Graphics API | **Vulkan only**, version **1.4** | no GLES/pixman fallback; explicit minimum requirement (§7.1) |
| Towards wlroots | our renderer also presents itself as a **`wlr_renderer`** | what wlroots draws by itself (hardware cursor, screencopy, output captures, uploading app buffers) uses our device and our shaders; no second renderer (§7.2) |
| Blur | **always live** | whatever is behind gets blurred on every frame in which it changes; it must be cheap by construction (§8) |
| Default scale | **chosen by Vela from the monitor's DPI**, in 25% steps | like Windows; the user can change it (§3.8) |
| X11 apps at fractional scale | **upscaled by the compositor** (right size, slightly blurry) | later, a setting to let them scale by themselves, as in Plasma (§3.9) |
| Battery | **no reduction of effects** | modern laptops handle everything; no automatic "power saving" mode (§8.3) |
| VRR | **postponed**, but the design must not rule it out | §4.4 |
| Vela's title bar | **designed here**, drawn by the compositor | §9 |
| Title bar tint | **derived from the wallpaper**, like Mica | §9.4 |
| Title bar font | **KDE's**, for now | §9.5 |
| Icons | **always SVG**, drawn at the exact physical size | §9.6 |

wlroots remains the "plumbing": backends (DRM/KMS, session, libinput, nested,
headless), `wlr_output` (modes, atomic commits, VRR), the protocols (xdg-shell,
layer-shell, seat, dmabuf, syncobj, fractional scale, presentation, ...) and the
`wlr_buffer` abstraction. **Scene, drawing, frame timing and policy belong to
Vela.**

## 3. Coordinates and scale: the heart of sharpness

Most desktops get this wrong. Vela's rules:

### 3.1 Two spaces, a single conversion point

- **Logical space** (`double`): where windows, animations, layouts and input
  live. One logical unit = one pixel at 100% scale.
- **Physical space** (`int`): the real pixels of each output.
- The logical → physical conversion happens **in one place only**, per output,
  at drawing time. Never earlier, never twice.

### 3.2 Round the edges, not position and size

A logical rectangle `[x, x+w)` becomes physical as
`[round(x·s), round((x+w)·s))`. Rounding position and width separately produces
one-pixel gaps between side-by-side windows (snap!) or overlaps. With edges, two
rectangles adjacent in logical space always stay adjacent in physical space.

### 3.3 1:1 buffers, no resampling

- Each surface receives the exact preferred scale through `fractional-scale-v1`
  (in 120ths: 125% = 150/120, 150% = 180/120, 175% = 210/120, all exact) and,
  with `viewporter`, draws a buffer as large as its **physical** area.
- The renderer recognizes when a buffer maps 1:1 onto the physical pixels it
  falls on, and then copies it **without filtering** (nearest sampling, integer
  coordinates): the result is bit for bit the app's buffer.
- Only when the sizes don't match (old apps with integer scale, scale
  animations) is there resampling, with a quality filter: bicubic or 4-tap
  Lanczos for upscaling, mipmaps for downscaling. Never the "default" bilinear
  filter that softens everything.

### 3.4 Positions snapped to physical pixels

**How it is done (S2).** A surface whose buffer is as large as its physical
area (up to rounding) snaps to the nearest physical pixel and occupies exactly
the buffer's pixels: 1:1 copy, nearest sampling. Rounding the two edges
separately at 150% sometimes gave a box one pixel larger than the buffer, and
the whole window went through the filter.

The **physical** position of each window (the origin of its main surface) is an
integer. Animations compute fractional logical positions, but on every frame the
origin snaps to the nearest physical pixel: no text "trembling" by half a pixel
during a movement. Only scale transforms (opening, minimizing) resample, and only
for their short duration.

### 3.5 Well-chosen logical sizes

**How it is done (S2).** Snap, maximize and fullscreen start from the area in
physical pixels (halves are split in pixels); the window's logical position is
the exact one, fractional if needed, and the integer size is chosen so that the
client's buffer (round(size × scale) in 120ths) covers exactly those pixels.
When that is impossible (at 150%, 2560 pixels would be 1706.67 units), the extra
pixel spills **off screen** if the area touches an edge with no other output next
to it; otherwise it stays one pixel inside, without overlapping anything else.

With fractional scales a window can have a logical size that does not become a
whole number of physical pixels. When the compositor **decides** a size
(maximized, snapped, fullscreen), it chooses it so that the physical edges fall
exactly on the edges of the area: start from physical pixels and go back to
logical, not the other way around. A half-screen snap on a 2560 px monitor at
125% is 1280 physical pixels per side, not "1024 logical and then we'll see".

### 3.6 Multiple outputs with different scales

- Each output has its own scale; a window straddling two outputs gets as its
  preferred scale that of the output showing the larger part of it, and is
  resampled with the quality filter on the others.
- The cursor has the right size on every output (cursor theme loaded for every
  scale in use).

### 3.7 The shell

(S2: Qt 6 already uses `fractional-scale-v1` and the PassThrough rounding policy
by default; verified sharp at 125% nested in KDE.)

The Qt shell uses `fractional-scale-v1` and `viewporter` for layer-shell
surfaces too (Qt ≥ 6.5), with `QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough`. The
theme's measurements are logical; borders and thin lines must be drawn aligned
to physical pixels.

### 3.8 The default scale

Like Windows, Vela chooses the scale of each output by itself the first time it
sees it; the user can then change it (Settings, and `VELA_SCALE`).

- DPI = diagonal in pixels / diagonal in inches (physical size from the EDID).
- Scale = DPI / reference, rounded to the nearest 25%, between 100% and 300%.
  The reference is 96 DPI for external monitors and 105.6 (96 × 1.1) for
  internal laptop panels, which are looked at from closer up.
- Missing or absurd physical sizes (projectors, TVs, some adapters): 100%.

Results of the formula on typical screens (a starting point, to be tuned with
real monitors):

| Screen | DPI | Scale |
|---|---|---|
| 24" 1920×1080 | 92 | 100% |
| 27" 2560×1440 | 109 | 125% |
| 32" 2560×1440 | 92 | 100% |
| 34" 3440×1440 | 110 | 125% |
| 27" 3840×2160 | 163 | 175% |
| 32" 3840×2160 | 138 | 150% |
| 15.6" laptop 1920×1080 | 141 | 125% |
| 14" laptop 1920×1200 | 162 | 150% |
| 16" laptop 2560×1600 | 189 | 175% |
| 14" laptop 2880×1800 | 243 | 225% |

### 3.9 X11 apps

X11-only apps (Xwayland) don't know about fractional scaling. Decision: **they
draw at 1× and the compositor upscales them**, with the quality filter of §3.3:
they have the right size and are slightly blurry, as on Windows. Later a
setting, as in Plasma, will let them scale by themselves (Xwayland at scale 1
and DPI communicated via Xft/XSETTINGS): sharp, but it depends on how well each
X11 app supports scaling.

### 3.10 How we prove it

Automated tests (headless session + `vela-shot`) with a **test client** that
draws "nasty" patterns: alternating one-pixel lines, checkerboards, 1 px text.
At every supported scale the test compares the pixels on screen with the
client's buffer **bit for bit**, also after moving, snapping and maximizing it.
A single differing pixel fails the test.

### 3.11 The magnifier

The magnifier (Accessibility, Win+Plus) is not an effect applied to the
finished image: the output under the cursor builds the scene starting from
another point of the layout and at a larger scale (`vela_output_frame_set_magnifier`).
Each surface is therefore drawn from its own buffer at the magnified size, with
the bicubic filter of §3.3, and text stays more readable than when enlarging
pixels that were already drawn. Damage is found by comparing with the previous
frame (§6): moving the zone changes everything. The cursor goes where the point
it indicates is shown (`wlr_output_cursor_move`); the zone follows it when it
reaches the edges, and a change of magnification keeps the point under the
cursor still.

## 4. Time and frames: any refresh rate

### 4.1 One clock per output

Each output has its own frame cycle, independent from the others: a 60 Hz and a
240 Hz monitor connected together each work at their own pace.

### 4.2 Animations look to the future

Originally, animations used the moment the "frame" event arrived. The renderer
instead uses the **predicted presentation time** of the frame being drawn (last
real vblank + period, from the backend's presentation feedback). This way motion
is correct to the frame, even at 360 Hz, even with VRR, even when a frame arrives
late.

**Output latency is learned** (discovered in S0). Several vblanks can pass
between handing over a frame and its appearance: 1 or 2 in a window nested in
KWin, depending on how KWin is working; on some drivers or with some planes, on
hardware too. The `FrameClock` compares each presentation with the prediction:
if the last 8 measurements agree on an offset of N whole periods, it adds N to
the latency. Measured in S0: at 180 Hz nested in KWin, after learning, a mean
error of 1 µs.

### 4.3 Drawing late (late latching)

The renderer measures how much CPU+GPU time a frame needs (a high percentile of
recent measurements) and starts drawing **that time plus a margin before the
vblank**, not right after the previous vblank. Input and animations sampled
later = lower latency. If the estimate is wrong and a vblank is missed, the
margin grows by itself.

**How it is done (S3).**

- **The cycle.** When the backend says "frame" (at the vblank after a
  submission, or immediately if the output was idle), `FrameClock::plan` chooses
  the vblank to draw for, the first one reachable by starting at least "cost +
  margin" before it, and a timer (`timerfd`, with the process timer slack at
  1 ns) wakes the compositor at that moment. Requests arriving in the meantime
  (app commits, input) end up in the same frame.
- **The cost** of a frame is measured from the moment it was supposed to start
  to the moment it was ready: commit done *and* GPU finished. The GPU's end is
  read with GPU timestamps (`vkCmdWriteTimestamp2`) brought onto
  `CLOCK_MONOTONIC` with `VK_KHR_calibrated_timestamps`. Without calibrated
  timestamps, the duration of the GPU work plus the commit is used. The cost
  used is the maximum of the last second (at least 8 frames): one slow frame
  must not make the following ones miss their vblank.
- **The margin** starts at 1 ms (`VELA_LATCH_MARGIN`). A frame that shows up late
  makes it grow immediately (+max(0.25 ms, period/10)); then it comes back down
  by 0.25 ms per second. The first second after startup or a mode change doesn't
  count (allocations and modesets make a few frames late, once). The budget
  (cost + margin) never exceeds "period − min(0.5 ms, period/4)": right after a
  vblank there is always time for the next one, so the refresh rate is never
  halved.
- **Who is late.** If the frame arrives late and the margin can still grow, the
  delay is ours (margin). If the margin is already at its maximum, the delay
  belongs to the output and is learned as latency (§4.2). Once a higher latency
  is learned, the extra margin is reset.
- **Never a commit without a buffer.** With DRM a commit without a buffer
  blocks: it stops the compositor until that output's vblank. When there is only
  the cursor to move, a buffer is submitted anyway (with empty damage, drawing
  costs almost nothing). Frames are requested without
  `wlr_output_schedule_frame`, which would mark the output as needing a commit
  even when nothing changes on it. This came up in the first test from a TTY: a
  75 Hz monitor next to the 180 Hz one blocked for ~12 ms on every movement and
  dragged the 180 Hz below 60 fps.
- **Priority.** The main thread asks for realtime scheduling (`SCHED_RR` at
  priority 10, not inherited by the shell and apps; it needs `RLIMIT_RTPRIO` or
  `CAP_SYS_NICE`, and `VELA_REALTIME=0` turns it off). The GPU queue asks for
  high priority (`VK_KHR_global_priority`), which amdgpu only grants with
  `CAP_SYS_NICE` (as for `kwin_wayland`); without it, it stays normal.
- **The virtual vblank** of the headless backend simulates a real output: a
  frame appears at the first vblank at which it was ready (the GPU must have
  finished too); if it wasn't, the previous one stays and the vblank is missed.
- **The statistics** (`VELA_DEBUG=1`, every 2 s) report: cost, margin, average
  time **from drawing to light** (the latency the compositor adds to input and
  animations), prediction error, missed vblanks. `VELA_LATCH=0` turns late
  latching off for comparison.

Measurements (headless, vkcube, RX 9070 XT): from drawing to light 1.1–1.9 ms at
every refresh rate between 60 and 360 Hz, against a whole period without late
latching (2.78 ms at 360 Hz, 16.7 ms at 60 Hz).

**Nested in KWin** the margin adapts to the host. KWin at 180 Hz composites right
after the vblank, so the margin rises to its maximum and drawing happens as soon
as the frame callback arrives. Isolated frames (text typed into a window) now
have the same exact prediction as animations: an error of 0.001 ms, against up to
one period in S1.

### 4.4 VRR

With VRR there is no fixed vblank: the monitor refreshes when the frame arrives,
within its limits. Animations stay correct thanks to §4.2 (predicted
presentation time, not "a fixed period").

- **When**: the choice is in vela.conf ("variable-refresh"): `games`
  (default: only with a fullscreen app on that output, like KWin's "Automatic",
  because with some monitors the desktop flickers), `always` or `no`;
  `VELA_VRR=1/0` wins. Each output's frame cycle checks on every frame whether it
  should be on or off and puts it in that frame's commit (tested first: if the
  monitor doesn't accept it, this is remembered and not retried).
- **How**: with a game in direct scanout (§5.3) the frame is submitted as soon as
  the game commits, without planning late latching (§4.3) on a vblank that
  doesn't exist; otherwise the cycle stays the usual one, which with VRR on and
  regular frames behaves as at a fixed refresh rate.

### 4.4.1 Tearing

A fullscreen game can ask with `wp-tearing-control-v1` to show each frame as soon
as it is ready, even mid-screen. This is only granted in direct scanout (§5.3),
with DRM's asynchronous page flip (`tearing_page_flip`); if the driver doesn't
accept it, the frame goes with the vblank as usual. While it lasts, that output's
frame cycle doesn't plan late latching (§4.3): it draws (that is, submits the
game's buffer) as soon as the game commits. It is turned off in Settings
("tearing" in vela.conf) or with `VELA_TEARING=0`.

### 4.5 Frame callbacks and feedback

- `wl_surface.frame` is sent after the frame **has been presented**, only to the
  surfaces visible on that output; a surface on two outputs follows the "main"
  output (the one with the larger part; on a tie, the faster one).
- `wp_presentation` receives the real presentation times.
- Hidden surfaces (covered, minimized, on another workspace) don't receive
  callbacks: apps stop drawing and consuming.

## 5. The scene

### 5.1 What it contains

- **Windows** (xdg toplevels), **shell surfaces** (layers), **popups**.
- **Vela's nodes**: rectangles, snapshots (closing and minimizing animations),
  snap previews.
- Each node has: a transform (translation + scale around a point), opacity, a
  clip with a **rounded rectangle**, a shadow, background blur, visibility.

### 5.2 Surfaces are read live

The scene **does not copy** an app's tree of surfaces and subsurfaces: at drawing
time it walks it directly from wlroots (`wlr_surface`, `wlr_subsurface`,
synchronized states already resolved by wlroots). Less duplicated state = fewer
bugs. The scene only keeps what is ours: position, effects, animations.

What is live is always ready: a commit whose buffer the app's GPU hasn't
finished drawing doesn't become the surface's current state until its fence is
signaled (§7.3). Reading the current state never means waiting for an app.

### 5.3 What `wlr_scene` did and we now do

| Task | How | Status |
|---|---|---|
| Buffer import | shm → our texture, re-uploaded only where the app drew (reusing wlroots' `wlr_client_buffer`); dmabuf → direct import (§7.3), kept while the buffer is in use | ✔ S1 |
| Damage | on every frame the flattened scene is compared with the previous frame (elements that appeared, disappeared, moved, changed, rose above others); surface content comes from commits, with the app's precise damage; buffer age with `wlr_damage_ring` | ✔ S1 |
| Occlusion | what is covered by opaque regions is not drawn and receives no frame callbacks | ✔ S1 |
| Frame callbacks, presentation | §4.5: only to visible surfaces, from the output showing the larger visible part | ✔ S1 |
| Output enter/leave | for each surface and subsurface, from the outputs it falls on; a surface hidden everywhere receives no leave (when it reappears nothing changes for the app) | ✔ S1 |
| Preferred scale | from the output showing the larger part (§3.6), integer and fractional | ✔ S1 |
| Captures | windows (`ext-image-capture`, Alt+Tab previews): the renderer draws only that window; outputs (screencopy, `ext-image-copy-capture`): through our `wlr_renderer` | ✔ S1 |
| Snapshots | locked buffers drawn as scene nodes | ✔ S1 |
| Cursor | hardware, through wlroots (with our renderer); drawn by us when there is no cursor plane | ✔ S1 |
| dmabuf feedback | per surface: the scanout tranche when the window is fullscreen (after 30 consecutive frames as a candidate, like `wlr_scene`), the default one again when it stops | ✔ S3 |
| Direct scanout | a single visible, opaque surface (format without alpha, or an opaque region covering everything) covering the output 1:1 with its transform, with no cursor drawn by us and no capture in progress → its buffer goes straight to the primary plane; with explicit sync the backend waits for the acquire point and signals the release when it stops showing it | ✔ S3 |
| YUV formats (video) | NV12 and similar, with conversion in the shader | S7 |

## 6. The frame, step by step

For each output, when the time comes (§4.3):

1. **Time**: the predicted presentation time is computed; animations advance to
   that moment.
2. **Draw list**: the scene is flattened, from bottom to top, into a list of
   "quads" in that output's physical coordinates: texture or color, rounded
   clip, opacity, transform, filter (nearest if 1:1, quality otherwise),
   effects.
3. **Occlusion**: whatever is completely covered by opaque surfaces is discarded
   (opaque regions declared by clients + our own opaque nodes).
4. **Damage**: the region to redraw = damage accumulated over the age of the
   destination buffer, **enlarged** where blur requires it (§8.3).
5. **Direct scanout?** If possible (§5.3), no drawing: the client's buffer is
   presented.
6. **Drawing** in the damaged region, in a single command buffer: background →
   for each element: the background blur under it, if any, then the shadow,
   then the clipped content.
7. **Presentation**: atomic commit with the end-of-rendering fence; at the
   vblank, frame callbacks and presentation feedback.

The CPU scene comparison has a [standalone benchmark](scene-diff-benchmark.md)
for stable scenes, movement, reordered and replaced elements, and multiple
outputs. Its match flags are reused per output; only growth allocates memory.
The search still starts at the expected position and can be quadratic for
heavily changed lists. The benchmark measures that cost separately from the
GPU and frame presentation.

## 7. Vulkan

### 7.1 Minimum requirement

**Vulkan 1.4** (dynamic rendering and synchronization2 from 1.3, timeline
semaphores from 1.2, push descriptors from 1.4: textures are bound on every draw
without descriptor pools) plus the extensions needed to exchange buffers with
the kernel and with clients:

- `VK_EXT_image_drm_format_modifier`, `VK_EXT_external_memory_dma_buf`,
  `VK_KHR_external_memory_fd`, `VK_EXT_queue_family_foreign`
- `VK_KHR_external_semaphore_fd` (sync_file, for implicit and explicit sync)
- `VK_EXT_physical_device_drm` (to choose the GPU driving the output)
- optional: `VK_KHR_calibrated_timestamps` (when the GPU finishes a frame, for
  late latching §4.3; without it, only the duration is measured)

In practice: AMD GCN and later (RADV) and Intel Gen9/Skylake and later (ANV)
with Mesa ≥ 25.0, NVIDIA with the proprietary driver ≥ 570 or NVK. Without
Vulkan 1.4 Vela can't be used: this is written in the README and reported at
startup with an understandable message, not a crash.

### 7.2 A device of our own

Vela creates its own `VkInstance`/`VkDevice` on the output's GPU
(`VK_EXT_physical_device_drm` compared with the backend's DRM device). We don't
use the wlroots renderer for drawing. To allocate output buffers we write our
own `wlr_allocator` (`wlr_allocator_init` is public): GBM buffers with an
explicit modifier accepted by the primary plane
(`wlr_output_get_primary_formats`).

**Our renderer also speaks wlroots' language** (chosen in S1). wlroots draws by
itself in a few places: it prepares the hardware cursor buffer, copies the
output for screencopy and `ext-image-copy-capture`, and uploads apps' `wl_shm`
buffers into textures on every commit (only the changed part, with the
`wlr_client_buffer` reuse logic). Instead of keeping a second renderer for this,
our renderer also implements the public `wlr_renderer` interface
(`wlr/render/interface.h`): textures, render passes and pixel reads are ours,
with the same device, the same shaders, the same blending in linear space and
the same synchronization. The code is in `compositor/src/render/`:

| File | What it does |
|---|---|
| `renderer.h` | the public API the rest of the compositor uses; `render.h` holds the structs the render files share |
| `vulkan.c` | device, importable formats (textures, render targets, wl_shm), dmabuf import |
| `renderer.c` | GPU submission with a timeline semaphore, deferred destruction (typed lists of retired images and used semaphores, freed when the timeline reaches their point), staging memory for uploads, dmabuf render targets, pipelines; syncobj timeline for app releases and GPU timestamps (S3); the `wlr_renderer` |
| `texture.c` | textures from dmabuf (one import per buffer) and from memory (upload of changed areas only), pixel reads |
| `pass.c` | drawing: textured or solid-color quads, rectangle clipping, barriers, implicit and explicit sync, timing; the `wlr_render_pass` |
| `allocator.c`, `formats.c`, `pixel_buffer.c` | GBM output buffers, the pixel format table, CPU images as `wlr_buffer` |

An output's time (vblank grid, learned latency, late-latching cost and
margin, the plan of each frame, §4) is `compositor/src/frame_clock.c`.

The scene and the frame are in `compositor/src/scene/` (§5, §6).

### 7.3 Client buffers and synchronization

- dmabuf: imported with the client's modifier, cached per buffer; ownership
  transfer from the "foreign" queue on every use.
- Implicit sync: sync_file extracted from the dmabuf
  (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`) and waited on as a semaphore; at the end, a
  sync_file is put back into the dmabuf.
- Explicit sync (`linux-drm-syncobj-v1`, already in wlroots 0.20): wait and
  release points on the client's timelines.
- Towards the output: the end-of-rendering fence passed to the commit.

**How it is done (S3).** The renderer has its own syncobj timeline (from the
render node): each draw signals a point on it by importing the end-of-work
sync_file. For each app using `linux-drm-syncobj-v1` that a draw reads (output
or capture), that point becomes one of its release points
(`wlr_linux_drm_syncobj_v1_state_add_release_point`): wlroots releases the buffer
to the app when every output that read it has finished and the app has sent
another one. The wait is the acquire point exported as a sync_file, instead of
the dmabuf's implicit fence. The renderer declares `features.timeline`, so
wlroots (captures) can also ask for wait and end-of-work points. Towards the
output, implicit sync remains, and it is enough. `WLR_RENDER_NO_EXPLICIT_SYNC=1`
turns the protocol off. Tested with vkcube and mpv (Vulkan, Mesa 26): the apps
run without stalling, so the releases arrive.

**NVIDIA 580 compatibility.** The proprietary 580 branch defaults to CPU
synchronization (`VELA_SYNC_FILE=0` behavior). This disables Vulkan sync_file
semaphore imports/exports and the client explicit-sync protocol. Implicit
producer fences are still exported, waited on and closed; rendering completes
before handing buffers back. Already completed fences are closed without a
Vulkan import on every driver. CPU waits retry interrupted polls and failed
waits prevent submission.

This is a conservative mitigation for the Quadro P1000/580.178.04 report in
which the compositor exhausted descriptors, not a confirmed attribution of
the leak. NVIDIA users have reported [sync_file leaks with two EGL waits on
580.178.04](https://forums.developer.nvidia.com/t/linux-drivers-egl-bug-sync-file-fd-leaked-when-two-eglwaitsync-calls-are-issued-on-one-context/384832)
and [explicit-sync EGL leaks on 580](https://github.com/NVIDIA/egl-wayland/issues/196).
Those reproductions use EGL, while Vela uses Vulkan. The mitigation avoids the
driver's external semaphore path at the cost of CPU/GPU overlap; it still needs
verification on affected NVIDIA hardware. Mesa/NVK and other NVIDIA branches
keep the existing policy. `VELA_SYNC_FILE=1` opts back into GPU fence exchange
when supported; `VELA_SYNC_FILE=0` forces CPU synchronization on other GPUs for
comparison. Both paths have a sustained 1200-frame FD regression scenario.

Do not close an FD after a successful Vulkan semaphore import: [Vulkan transfers
ownership to the implementation](https://docs.vulkan.org/refpages/latest/refpages/source/vkImportSemaphoreFdKHR.html).
Only completed fences skipped before import and failed imports are closed by
Vela.

**Commits wait for their fences (after S6).** A frame must never wait for an
app's GPU: one late app would make the whole output miss vblanks, cursor and
animations included. So a commit with a buffer that isn't ready yet is held back
(`wlr_surface_lock_pending`) and applied when its fence is signaled; meanwhile
the surface keeps its previous, ready state, which is what every frame draws
(`scene/ready.c`). The fence is:

- with explicit sync, the acquire point of the commit being held. wlroots 0.20
  only exposes the current `linux-drm-syncobj-v1` state; the pending one is the
  same synced object's state for `surface->pending`, found in the surface's
  list of synced objects (private in wlroots: if it changes, the build stops
  there). wlroots itself only waits for the point to *materialize*; we wait for
  it to be *signaled*, with a `DRM_IOCTL_SYNCOBJ_EVENTFD` in the event loop;
- with implicit sync, the dmabuf's write fences (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`
  for reading, one per distinct plane fd), polled in the event loop;
- shm buffers are always ready.

Commits stay in order (wlroots applies cached states one after another), frame
callbacks leave with the commit that carried them, so the app is naturally paced
by its own GPU. `VELA_READY_WAIT=0` turns it off, for comparison. A limit: a
synchronized subsurface whose buffer is late reaches the screen after its
parent's commit, instead of together with it.

**Imports last only as long as the buffer is in use.** Holding commits was not
enough for implicit sync: RADV makes every `VkDeviceMemory`, imported dmabufs
included, resident in *all* of the device's submissions, and the amdgpu kernel
driver makes a submission wait for the write fences of every implicitly synced
buffer in it. A dmabuf kept imported in a cache while its app redraws it (it was
released, it isn't on screen) therefore blocked every frame until the app's GPU
finished. So a client dmabuf's import is freed as soon as wlroots stops using it
(when the GPU is done reading it) and redone if the app sends it again: 9 µs per
import (median; 40 µs at most), against whole frames lost.

Measured (headless at 60 Hz, `tools/vela-slowgpu`: an app whose GPU work, on a
compute queue so as not to compete for the graphics one, ends ~150 ms after the
commit; the cursor moving for 1 s):

| | missed vblanks | frames |
|---|---|---|
| explicit sync, before | 47 | 8 |
| explicit sync, now | 0 | 59 |
| implicit sync, before | 47 | 9 |
| implicit sync, holding commits only | 36 | 19 |
| implicit sync, now | 0 | 58 |

`tests/functional/test_ready.py` repeats the measurement on every run, and also
checks that without the wait the output really does stall.

### 7.4 Shaders and pipelines

- GLSL compiled to SPIR-V **at build time** (`glslc`) and embedded in the
  executable. No shader compiler at runtime.
- Few, generic pipelines: textured quad (rounded clip, opacity, chosen filter),
  solid-color quad, analytic shadow, blur downsample/upsample, "acrylic"
  composition.
- No heavy libraries: Vulkan's C API, used from plain C.
  Our own memory allocation (few, long-lived resources; client buffers are
  external memory). VMA will be considered only if it is really needed.

### 7.5 Color

- Blending in **linear space** (`_SRGB` views or fp16 intermediates), not on sRGB
  values as almost every compositor does: correct gradients and transparency.
- 10-bit outputs where available.
- Later HDR and `color-management-v1` (already in wlroots 0.20): a linear
  pipeline from the start makes it an extension, not a rewrite.

### 7.6 Night light and color filters

Both are a 3x3 matrix in linear space (night light is a diagonal one: how much
red, green and blue remains at the chosen temperature).

- **Color filters** (grayscale, color-blindness corrections): applied while
  drawing. The matrix goes into each quad's constants (`QuadPush.filter`, bit 2
  of the flags) and is applied by the texture, rectangle and shadow shaders.
  Since it is linear, and blending is a linear combination in linear space
  (§7.5), applying it to every draw is equivalent to applying it to the finished
  image, with no extra pass. The blur reads a background that already has the
  filter: in the acrylic composition only the tint gets it. With a filter on
  there is no direct scanout, and the cursor is drawn by the renderer (so it
  gets the filter too).
- **Night light**: on real outputs in the **monitor's gamma**
  (`wlr_output_state_set_color_transform` with a 3x1D table: each encoded value
  goes back to linear light, takes the gain and is re-encoded), tried first with
  a test commit and then sent with the frame. This way it doesn't end up in
  screenshots or screen sharing, it also applies to the hardware cursor, and
  games keep direct scanout. Where there is no gamma (nested, headless) or the
  monitor doesn't accept it, it is multiplied into the drawing matrix like the
  filters.
- The transition (one second, like Windows) is an animation of §4.2: a new value
  on every frame, and the whole output redrawn (or just a new gamma).

## 8. Effects

### 8.1 Rounded corners

Clipping with the distance from a rounded rectangle (SDF) computed in the
shader, in **physical pixels**, with antialiasing of exactly one physical pixel.
Sharp at every scale. It applies to the window's frame (not to shadow margins
drawn by the app).

How it is done: a scene tree can have a **shape** (`struct vela_shape`: rectangle,
radius, shadow); the elements of its children inherit the clip (popups don't:
`vela_node.unclipped`). The window computes its own on every frame
(`vela_view_update_shape`): radius 8, none when maximized, fullscreen or snapped,
nor for apps with their own shadow margins (GTK). Corners don't count as opaque;
with corners there is no direct scanout. Animation snapshots carry the shape
with them. The sharpness test stays bit for bit outside the corner squares.

### 8.2 Shadows

The analytic shadow of a rounded rectangle (closed-form integral of a Gaussian,
Evan Wallace's technique): one quad, no texture, no blur. Two layers like
Windows 11 (a wide, soft shadow + a tight contact shadow), stronger for the
active window. Apps with their own decorations already draw their shadow: if the
xdg geometry indicates shadow margins, ours is not drawn.

How it is done: two elements per window (wide: sigma 14, offset 8 downwards;
contact: sigma 2), darker for the active one, under the content. The shader
doesn't draw under the window, and its clip excludes the interior (except the
corners): it costs a frame, not the whole area.

### 8.3 Live blur

- **What gets blurred**: the areas a client asks for with the standard
  `ext-background-effect-v1` protocol (the shell for the taskbar, Start menu,
  Alt+Tab, notifications; apps that support it) and our server-side
  decorations. Not "everything transparent": apps' shadow margins are
  transparent and must not be blurred.
- **Algorithm**: dual Kawase (a chain of downsamples and upsamples at half
  resolution), then the acrylic recipe: saturation, tint, luminosity and a veil
  of noise to avoid banding. The radius is in logical units, so it is identical
  at every scale.
- **Cost under control**:
  - only the **damaged** part behind each area is blurred, enlarged by the
    radius;
  - the blurred background of each area is **cached**: if nothing changed
    behind the taskbar (the normal case), the blur is not recomputed at all;
  - a change behind a blurred area damages the area itself as well (this is how
    the blur "follows" what moves behind it).
- Blurs on top of each other (the Start menu over the taskbar) compose in the
  right order because drawing goes from bottom to top.

How it is done: Vela implements the protocol itself (`scene/effects.c`;
wlroots doesn't have it). While drawing, upon reaching a panel with a region to
blur, the render pass is closed, what is already drawn underneath is read (the
output's buffer, also imported as a texture if the format allows it), four
dual Kawase downsamples and three upsamples run in 16-bit linear images that grow
when needed and are reused, then the acrylic recipe is composed and drawing
resumes. The shape comes from the panel's alpha: the shell asks for rectangles,
and rounded corners stay. Damage touching an area (with the radius) makes it
redraw entirely, radius included. Measured cost: 0.05–0.13 ms of GPU per frame
with the Start menu opening over the taskbar. The cache of the blurred
background (keeping it from one frame to the next if nothing changes behind) has
not been needed yet: at idle nothing is redrawn anyway.
- **Everything at full quality, even on battery**: no automatic reduction of
  effects. Savings come from not drawing what doesn't change, not from removing
  effects.

## 9. Vela's title bar

Originally Qt/KDE apps drew Qt's "fallback" title bar by themselves, because
Vela offered no server-side decorations. Vela's title bar is the first "own"
element bringing together everything the renderer can do: text, blur, corners,
shadows, animations.

**Second version (October 2026)**: for Wayland apps too, with `xdg-decoration`
(Vela offers the server-side bar to apps that don't ask for their own; Qt and KDE
accept it, GTK4 doesn't). The bar is part of the geometry for them too, and apps
get the part below. On the left, the app icon (from the `.desktop` file via
`app_id` or `StartupWMClass`, from KDE's icon theme, an SVG drawn with librsvg at
the physical size; without librsvg, which is optional, no icon): a click opens
the window menu, a double click closes the window. The background is the Mica
tint: the shell sends the compositor the average color of the desktop wallpaper
(`wallpaper-tint`), made safe for text and mixed with Windows 11's gray (more so
when inactive). The live blur behind the bar (§9.4, last point) is postponed:
the solid tint is already close to Mica, which on Windows doesn't show the
windows behind it.

**First version (September 2026)**, only for X11 apps that leave the title bar
to the window manager (`compositor/src/decoration.*`): measurements of §9.3,
fixed colors of Windows 11's dark theme (no blur or tint yet), the title with
FreeType + HarfBuzz in KDE's font rasterized at the output's physical scale,
button symbols drawn as antialiased segments at physical size. The bar is part of
the window's geometry (32 units above the surface), so snap, maximize and
placement include it; the X11 app only gets the part below. Dragging from the
title starts after 4 units of movement (a double click doesn't restore a
maximized window). Missing at the time: invisible resize borders and the app
icon.

### 9.1 Who uses it

- The `xdg-decoration` protocol lets the compositor say "I'll draw it". Vela
  prefers that, except when the app explicitly asks to draw its own: Chromium
  and Brave with tabs in the title bar draw it anyway, and imposing ours gave
  duplicate buttons.
- Qt/KDE apps accept it: an immediate, visible gain.
- Firefox and Chromium let the user choose; GTK4/libadwaita apps always draw
  their own bar and can't be persuaded. For them the compositor's rounded
  corners and shadow still apply.

### 9.2 Who draws it: the compositor

Two ways:

- **The shell (Qt) draws the bars** as separate surfaces. QML is reused, but on
  every resize two processes must present in the same frame: if they fail to, the
  bar and the window "come apart" for a frame. That is exactly the kind of defect
  Vela must not have.
- **The compositor draws the bar** in the same frame and with the same geometry
  as the window. No synchronization between processes, text rasterized directly
  at the output's physical scale. It needs a small text engine in the
  compositor (FreeType + HarfBuzz + fontconfig, already present on every system:
  Qt uses them too).

**Proposal: the compositor.** The text engine will be needed anyway for the
debug overlay (§10) and for other compositor UI.

### 9.3 What it looks like

- **Measurements** like Windows 11: a 32-logical-unit bar, 46×32 buttons,
  corners with radius 8, invisible 8-logical-unit resize borders outside the
  window.
- **Background**: live blur of what's behind, with a **tint derived from the
  desktop wallpaper** (like Mica on Windows): see §9.4.
- **Minimize/maximize/close buttons**: glyphs drawn as SDFs (lines), sharp at
  every scale; animated hover; red "close".
- **Title**: text with grayscale antialiasing (no RGB subpixel: it doesn't get
  along with transparency and animations), glyphs rasterized for each physical
  scale and never enlarged, in a texture atlas. The font is **KDE's** (§9.5).
- **App icon**: **always SVG** when the theme offers it (§9.6).
- **A single shape**: the bar and the app's content are clipped together as one
  rounded rectangle, with the shadow around the whole. When maximized or snapped,
  no corners on the sides touching the edges.
- **Interactions**: drag to move (with snap), double click to maximize, right
  click for the window menu (§14.8); later, with the mouse on the "maximize"
  button, Windows 11's snap layouts.

### 9.4 The tint, from the desktop wallpaper

Like Mica on Windows, the bar "picks up" the colors of the wallpaper behind it,
so every window blends with the desktop without any settings:

- when the wallpaper changes, the compositor derives a tiny version of it (a grid
  of a few colors, for example 32×18, from its texture's mipmap chain: the
  wallpaper is a shell surface the compositor is already drawing);
- a bar's tint is that grid read at the bar's position: it updates while the
  window moves, at the cost of one value passed to the shader;
- the color is then **made safe for text**: reduced saturation and luminosity
  brought into the theme's band (dark or light), so that the contrast with the
  title stays at least 4.5:1 (the WCAG threshold);
- final result = live blur of what's behind, mixed with the tint, plus a veil of
  noise; when inactive, the tint weighs more and the bar "dims", as on Windows.

### 9.5 The font, from KDE

For now the title uses the font chosen in KDE, so bars are consistent with Qt
apps: `activeFont` in the `[WM]` section of `kdeglobals`, otherwise `font` in
`[General]`, otherwise KDE's default (Noto Sans 10 pt), found with fontconfig.
Sizes in points become logical pixels at 96 DPI (10 pt = 13.33 px) and then
physical pixels with the output's scale. The font is reloaded when `kdeglobals`
changes.

### 9.6 Icons, always SVG

For sharpness at every scale, icons are drawn **from the vector**, at the exact
physical size: a 16-logical-unit icon is 20 pixels at 125% and 24 at 150%, drawn
directly at that size.

- The icon is looked up in the theme (freedesktop specification), always
  preferring the SVG version; PNGs are only used if the app has nothing else,
  choosing the largest and downscaling it with the quality filter.
- For drawing SVGs I propose **resvg**: it is the most faithful to the
  specification, has a C API, doesn't drag cairo and glib into the compositor and
  is in Arch's official repositories. The alternative, should it be missing in
  some distribution: lunasvg (C++, small) included in the source tree.
- Drawn icons are cached per (icon, physical size).

## 10. Measurements and tests

- **Timing**: GPU timestamps for every frame and every effect; CPU time; missed
  frames. `VELA_DEBUG_OVERLAY=1` shows them on screen.
- **Validation**: `VELA_VULKAN_VALIDATION=1` enables the validation layers.
- **Image tests** (headless): reference scenes compared with expected images;
  for sharpness, bit-for-bit comparison (§3.10); for effects, a small tolerance.
- **Test matrix**: scales of 100/125/150/175/200%, simulated refresh rates of
  60/75/144/165/240/360 Hz, multiple outputs with different scales.
- **Virtual vblank**: wlroots' headless backend doesn't simulate a faithful
  output (a millisecond timer, 1000000/refresh truncated: at 144 Hz it gives
  166 fps; and it "presents" at the moment of the commit). For headless outputs
  Vela uses its own nanosecond virtual vblank (`timerfd` with absolute deadlines
  on an exact grid): that is what refresh rates, prediction and missed vblanks
  are tested on. `VELA_OUTPUT_SIZE=1920x1080@144`.

## 11. Stages

The previous compositor (with `wlr_scene`) remained working until the new scene
matched it; then `wlr_scene` went away. Each stage closes only with its tests.

| Stage | Content | Done when |
|---|---|---|
| **S0** Foundations ✔ | our own Vulkan device, GBM allocator, import of output buffers, implicit sync (sync_file), compiled shaders, test scene; per-output frame cycle with predicted presentation time and learned latency; virtual vblank for headless | done: 60–360 Hz simulated exactly (0 missed vblanks, error < 10 µs); nested in KWin at real 75 and 180 Hz, error ~1 µs after learning; validation layers (synchronization included): no messages |
| **S1** Parity ✔ | own scene with windows, layers, popups, subsurfaces; shm and dmabuf; damage; frame callbacks, presentation, enter/leave; snapshots, snap and captures moved to the new renderer; Vulkan 1.4; our renderer also as `wlr_renderer` | done: `wlr_scene` and the wlroots renderer removed. Tested headless and nested in KWin: Konsole (shm), the Qt Quick shell and Firefox (dmabuf), popups including subsurfaces, Start menu, dragging, snap with preview, minimize and restore, closing, Alt+Tab with previews, screencopy, two outputs (enter/leave), 150% scale. Validation layers (synchronization included): no messages. 0 CPU at idle; dragging a window at 144 Hz, 17–29 ms of CPU over 3.4 s, against 34–36 ms for `wlr_scene` |
| **S2** Sharpness ✔ | fractional scale, pixel snapping, quality filters, multiple outputs with different scales, per-scale cursor, default scale from DPI | done: `scripts/test-sharpness.sh` bit for bit at 100, 125, 150, 175, 200 and 225% (opened, snapped left and right, maximized, restored); mixed outputs at 150% + 100%, bit for bit on the 150% one. Before the fixes: up to 134 thousand differing pixels at 150% for a centered window. Catmull-Rom bicubic filter for upscaling; downscaling still bilinear (mipmaps to do with scale animations, S4) |
| **S3** Time and latency ✔ | late latching, direct scanout, explicit sync, dmabuf feedback | done: headless at 60, 75, 144, 165, 240 and 360 Hz with vkcube: 0 missed vblanks, prediction error 0, from drawing to light 1.1–1.9 ms (a whole period without late latching). Shell, Konsole and a window dragged at 360 Hz: a single late frame (wlroots allocating a new swapchain buffer), absorbed by the margin. Direct scanout with fullscreen mpv: headless (OpenGL, implicit sync, frame cost 0.02 ms) and nested in KWin (Vulkan, explicit sync, with scanout dmabuf feedback); captures during scanout work. Validation layers (synchronization included): no messages. 0 CPU at idle |
| **S4** Shape ✔ | rounded corners, shadows | done: SDF in physical pixels and a two-layer analytic shadow; `scripts/test-sharpness.sh` bit for bit at 100–225% outside the corners; validation layers: no messages |
| **S5** Blur ✔ | `ext-background-effect`, dual Kawase, acrylic for the shell | done: taskbar, Start menu, menus, Alt+Tab and notifications blurred; 0.05–0.13 ms of GPU per frame while the Start menu opens; nothing at idle; validation layers: no messages. The blurred-background cache is postponed |
| **S6** Title bar ✔ | `xdg-decoration`, text engine with KDE's font, buttons, SVG icons, wallpaper tint, interactions | done: apps accepting xdg-decoration (Qt/KDE) have Vela's bar, with the app icon (SVG from KDE's theme with librsvg, at physical size) and the wallpaper's Mica tint; invisible resize borders. Postponed: live blur behind the bar (solid tint for now), animated hover, resvg instead of librsvg |
| **Ready commits** ✔ | commits wait for their fences; imports only while in use | done (§7.3): an app whose GPU ends 150 ms after the commit, explicit and implicit sync, headless at 60 Hz with the cursor moving: 0 missed vblanks (47 before). In the functional tests |
| **S7** Color | 10 bit, HDR, `color-management-v1` | |
| (later) VRR | refresh rate policy during animations | §4.4 |

## 12. Risks

- **The amount of work in S1**: redoing what `wlr_scene` gives for free is the
  longest part. Mitigation: parity verified with image tests against the current
  renderer, for as long as it exists.
- **wlroots changes its API at every minor version**: we use less of wlroots
  than before (no `wlr_scene`, no renderer), so less exposed surface.
- **Drivers**: dmabuf with modifiers and sync_file are well supported on
  AMD/Intel; NVIDIA must be tested early.
- ~~**Hardware cursor**: `wlr_output_cursor` uses the wlroots renderer to prepare
  the cursor plane's buffer.~~ Solved in S1: wlroots' renderer, as far as wlroots
  is concerned, is ours (§7.2).
- ~~**Prediction of isolated frames** (emerged in S1).~~ Solved in S3: with late
  latching every frame, isolated or not, starts at the same distance from the
  vblank (nested in KWin: an error of 0.001 ms).
- **Late latching and scanout on real DRM** (S3): late latching tested from a
  TTY (RX 9070 XT, 2560×1440 at 180 Hz at 125% + 1920×1080 at 75 Hz): dragging a
  window, 0 missed vblanks, 0.03 ms of CPU per frame, 0.01–0.03 ms of GPU work.
  Still to verify: scanout accepted by the primary plane, dmabuf feedback making
  apps change modifiers, a minimum margin under 1 ms.
- ~~**Slow apps hold the compositor back**~~ (emerged in S3). Solved after S6
  (§7.3): commits wait for their fences, and imported buffers are released when
  no longer in use. The original analysis: a frame waits (on the
  GPU, or in the kernel with scanout) for apps to finish drawing the buffers it
  shows. A late app can therefore make the whole output miss a vblank, cursor and
  animations included. KWin and Mutter apply an app's commit only when its buffer
  is ready, and in the meantime show the previous one. For us this means keeping,
  for each surface, the last ready state (today surfaces are read live, §5.2),
  and, for explicit sync, knowing the acquire point of a commit still pending,
  which wlroots 0.20 doesn't expose. **Decided (S4):** postponed to a stage of
  its own after S6. wlroots 0.20 has a way to hold back a commit
  (`wlr_surface_lock_pending`), but it doesn't expose the acquire point of a
  pending commit with explicit sync, which is exactly what modern apps (Mesa,
  Firefox) use: doing it only for implicit sync would cover the wrong apps. In
  the tests from a TTY, no vblank was missed because of an app.
- **Shutting down when nested** (emerged in September 2026): about one time in
  ten, closing Vela nested in KDE with an X11 app open, the amdgpu driver crashed
  while freeing GPU memory in the renderer's destructor (internal state already
  corrupted; RADV and Mesa's GBM share it within the process). Cause not found;
  mitigated by not tearing down the renderer and device on a normal shutdown
  (the kernel reclaims everything). With `VELA_VULKAN_VALIDATION=1` everything is
  torn down, to find forgotten resources.
- **Output rotation**: the frame handles it (elements in rotated space, damage
  and drawing brought back to the buffer), but it hasn't been tested on a real
  output yet.
- **Text and icons in the compositor** (§9.2, §9.6): new dependencies
  (FreeType, HarfBuzz, fontconfig, resvg) and delicate code. The first three are
  on every system; resvg must be checked on other distributions.
- **Multi-GPU** (hybrid laptops, outputs connected to the secondary GPU): outside
  S0–S2, but the "per output GPU" device (§7.2) must not prevent it.

## 13. Resolved questions

- Default scale from DPI → §3.8
- X11 apps upscaled → §3.9
- VRR postponed → §4.4
- No reduction on battery → §8.3
- Title bar in this document → §9
- Tint derived from the wallpaper → §9.4
- KDE's font, for now → §9.5
- Icons always SVG → §9.6
- Context menus copied from Windows 11, drawn by the shell → §14

New questions will come up while writing the code: they are added here.

## 14. Context menus

Vela copies Windows 11's context menus **in full**: same items, same order, same
groups, same behavior. Where an item makes no sense on Linux it is translated
into the closest equivalent (table in §14.11); it is removed only if no
equivalent exists.

> **Status (September 2026):** done: the component (`Menus`, `ContextMenu`,
> `MenuPanel` in the shell), the jump list, the taskbar's empty space, the clock,
> the system tray, Win+X, the Start menu's menus (with the "Pinned" section,
> which didn't exist before), the window menu with keyboard Move and Size, the
> desktop, text fields, Run (Win+R) and Win+D; then the desktop icons (the
> Desktop folder) with the desktop and file menus, icon row included, and "Show
> more options" with KDE's service menus, and the Properties window (General,
> Permissions, Details). Settings items (Personalize, Display settings, taskbar
> and notification settings, System, Win+I) open the right page of the Settings
> app (`vela-settings --page …`). Files (`vela-files`) uses the same component
> (`MenuPanel.qml`) inside its window, with the file menus and the folder's
> empty-space menu. Items waiting for their feature are present but disabled:
> Shortcut, Add to Favorites, "Choose another app". Menus for things that don't
> exist yet are missing: the Start menu's Recommended section, the notifications'
> "…" menu. Removed for lack of an equivalent: "Run as administrator" (graphical
> apps running as root under Wayland normally don't start), "App settings",
> "Share".
>
> *(Several of these have since been done — Shortcut, Favorites, "Choose another
> app", Share; see the README.)*

### 14.1 Who draws them: the shell

All menus are drawn by **the shell**, with a single Qt Quick component
(`VelaMenu`: menu, item, separator, submenu) used everywhere, so that every Vela
menu is identical to the others. The compositor doesn't draw menus: when a window
menu is needed (§14.8) it asks the shell. The system tray menu of the time
(`TrayMenu.qml`) was to move to this component.

The surface: for now **a fullscreen transparent window** in the overlay layer
(the one that had the system tray menu), on which menus and submenus are panels;
a click outside closes them. It works the same wherever the menu originates
(taskbar, Start, wallpaper, compositor) and on any output. An xdg popup with a
*grab* remains the alternative if needed (for example to avoid covering the
screen with the transparent window): to be reconsidered with blur (S5).

Two details that Wayland makes necessary:

- **The keyboard goes back to the panel.** The menu takes the keyboard; when it
  closes, the compositor gives it back to the shell surface that had it before
  (e.g. the Start menu), not to the active window.
- **The compositor knows about Shift.** When clicking the taskbar the keyboard
  belongs to another app, and the shell doesn't see modifiers: for Shift+right
  click it asks the compositor (`modifiers` on its socket, with a reply).

### 14.2 Appearance

Like Windows 11's menus (WinUI), in the dark theme:

- rounded corners (8), a thin border, a shadow; an acrylic background once blur
  exists (S5), until then the theme's `popup` color;
- each row: a 16×16 icon on the left (an empty column if no item in the group has
  an icon), text in KDE's font, the shortcut on the right in gray, an arrow `›`
  for submenus; check marks and bullets for choice items;
- highlight: a rounded rectangle (4) inset from the menu's edges; disabled items
  in gray;
- thin full-width separators between groups;
- in file menus (§14.9) a **row of icons** at the top or bottom (near the click
  point): Cut, Copy, Rename, Share, Delete;
- sharp at every scale like everything else (§3).

### 14.3 Behavior

- **Opening**: at the click point, with the top-left corner under the cursor; if
  there's no room it flips to the left or upwards. From the taskbar it rises
  upwards, above the button. With the Menu key or Shift+F10 it opens on the
  focused element.
- **Animation**: it enters with a fade and a short slide from the side it opens
  from, on the output's real frames (§4.2); it exits immediately.
- **Submenus**: they open on hover after a short delay (400 ms, like Windows), on
  click or with the right arrow; they stay open if the mouse goes there
  diagonally across other items.
- **Keyboard**: arrows, Home/End, Enter or Space to run, Esc closes one level,
  the left arrow goes back to the parent menu; the underlined letter (when opened
  from the keyboard) or the initial jumps to the item.
- **Mouse**: right-clicking an item runs it like a left click; a click outside
  closes the menu.
- **Touch**: a long press counts as a right click.

### 14.4 Taskbar: an app button (jump list)

The menu rising from the button, from top to bottom:

- **Pinned**: files pinned for that app (with the pin on hover to unpin them);
- **Recent**: the last files opened with that app (a pin to pin them; right click
  on a file: Open, Pin to this list / Unpin from this list, Remove from list);
- **Tasks**: the actions declared by the app (e.g. Firefox: "New window", "New
  private window");
- ---
- the app's name with its icon: opens another instance;
- "Pin to taskbar" / "Unpin from taskbar";
- "Close window", or "Close all windows" if there is more than one;
- "End task": closes the process without asking. In Windows 11 24H2 it must be
  enabled in settings; in Vela it is on by default (the user's choice), and it is
  turned off with `endTask=false`.

Shift+right click on the button opens the window menu instead (§14.8).

### 14.5 Taskbar: empty space, clock, icons

- **Empty space**: "Task Manager", "Taskbar settings".
- **Date and time**: "Adjust date and time", "Notification settings".
- **System icons** (done with quick settings): volume → "Open volume mixer"
  (quick settings' sound page), "Sound settings" ("Troubleshoot sound problems" has no equivalent); network →
  "Diagnose network problems" (disabled), "Network and Internet settings";
  battery → "Power and sleep settings" (to do together with the battery in the
  laptop taskbar).
- **App icons in the system tray**: the app decides the menu (dbusmenu, already
  done), drawn with `VelaMenu`.

### 14.6 Start button (Win+X)

Right click on the Start button or Win+X, with Windows 11's groups:

- "Installed apps", "Mobility Center" (laptops only), "Power Options", "Event
  Viewer", "System", "Device Manager", "Network Connections", "Disk Management",
  "Computer Management"
- ---
- "Terminal", "Terminal (Admin)"
- ---
- "Task Manager", "Settings", "File Explorer", "Search", "Run"
- ---
- "Shut down or sign out" › "Sign out", "Sleep", "Shut down", "Restart"
- "Desktop"

Win+X followed by the underlined letter opens the item directly.

### 14.7 Start menu

- **Pinned apps**: the jump list items (§14.4, recent files and tasks), then
  "Unpin from Start", "Move to front", "Pin to taskbar" / "Unpin from taskbar",
  "Run as administrator", "Open file location", "Uninstall". On pinned app
  folders: "Rename", "Unpin from Start".
- **All apps** and **search results**: "Pin to Start", "More" › ("Pin to
  taskbar", "Run as administrator", "Open file location", "App settings"),
  "Uninstall".
- **Recommended**: "Open file location", "Remove from list".
- **Search box** (and every text field in the shell): "Undo", "Cut", "Copy",
  "Paste", "Select all".

### 14.8 The window menu

Right click on the title bar (or on its icon), Alt+Space, or Shift+right click on
the taskbar button:

- "Restore", "Move", "Size", "Minimize", "Maximize"
- ---
- "Close" (Alt+F4)

Items that don't apply are disabled (e.g. "Restore" on a window that isn't
maximized). "Move" and "Size" work with the arrows and Enter, as in Windows.
Double-clicking the title bar icon closes the window.

The compositor asks for it: for Vela's title bar (§9), for Alt+Space, and for
apps with their own title bar that send `xdg_toplevel.show_window_menu`
(GTK/libadwaita do this on right click on their bar). The compositor passes the
window and the point to the shell (the shell's socket), and the shell draws the
menu; what it can't do by itself through foreign-toplevel (keyboard Move and
Size) it sends back to the compositor on the command socket.

### 14.9 Desktop

The icons are the files of the Desktop folder (`XDG_DESKTOP_DIR`) plus the Recycle
Bin, on the primary output (`DesktopModel` and `Wallpaper.qml`). Dragging them is
a real drag between apps (wl_data_device, handled by the compositor with its
icon): it can end in another app, on a folder, on the Recycle Bin or back on the
desktop, where the icons move. The compositor also keeps Wayland's **implicit
grab**: while a button is held, the pointer stays with the surface it was pressed
on, even on another output (without it, a selection rectangle leaving the output
never received the release).

- **Background**: "View" › ("Large icons", "Medium icons", "Small icons", "Auto
  arrange icons", "Align icons to grid", "Show desktop icons"); "Sort by" ›
  ("Name", "Size", "Item type", "Date modified"); "Refresh"; "Undo …" (the last
  action, Ctrl+Z); "New" › ("Folder", "Shortcut", then the document types);
  "Display settings"; "Personalize"; "Open in Terminal"; "Show more options".
- **Icons (files and folders)**: a row of icons (Cut, Copy, Rename, Share,
  Delete), then "Open", "Open with" ›, "Pin to Start", "Add to Favorites",
  "Compress to" › ("ZIP file", "7z file", "TAR file"), "Copy as path", "Open in
  Terminal" (folders), "Properties", "Show more options".
- **"Show more options"** (also Shift+F10 or Shift+right click): the full menu,
  with every item added by apps. On Linux these are KDE's *service menus* (and the
  actions apps declare for file types); in the modern menu only those that ask
  for it appear, like registered apps in Windows 11.

Files uses the same file menu (milestone 3).

### 14.10 Task View and notifications

With virtual desktops (milestone 3, done: `shell/qml/TaskView.qml`,
`compositor/src/workspace.c`):

- **A window preview**: "Snap left", "Snap right", "Move to" › (the desktops,
  "New desktop"), "Show this window on all desktops", "Show windows from this app
  on all desktops", "Close".
- **A desktop preview**: "Rename", "Choose background", "Move left", "Move
  right", "Close".

Notifications (the "…" menu of the popup and of the notification center): "Turn
off all notifications for <app>", "Go to notification settings".

### 14.11 From Windows to Linux

| Windows item | In Vela |
|---|---|
| Settings and its pages | Vela's Settings app (`vela-settings`), opened on the right page |
| Installed apps, Uninstall | Settings > Installed apps; uninstalling via Flatpak, Discover or the package manager |
| Mobility Center, Power Options | Settings > Power |
| Event Viewer | the systemd journal viewer |
| System | Settings > About |
| Device Manager | system information (kinfocenter) |
| Network Connections | Settings > Network and Internet (NetworkManager) |
| Disk Management, Computer Management | the partition manager; "Computer Management" opens the system information |
| Terminal / Terminal (Admin) | the default terminal / the same with a root shell requested through polkit |
| Run as administrator | only for apps that support it (polkit); graphical apps as root under Wayland normally don't start, so elsewhere the item isn't there |
| Task Manager | the system monitor (later Vela's own) |
| Run | a small command box, like Win+R |
| Pinned, Recent (jump list) | pinned files saved by Vela; recent files from `recently-used.xbel`, which records which app opened each file |
| Tasks (jump list) | the `[Desktop Action …]` actions in the app's `.desktop` file |
| Share | the sharing portal, once it exists; until then the item isn't there |
| Show more options | the full menu with KDE's service menus |
