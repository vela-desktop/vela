# Shell memory use

On the test desktop with 24 files and the Recycle Bin, idle shell PSS fell by
4.7 MiB. With 25 pinned screenshots, opening clipboard history used 132 MiB
less PSS. Image content, output scale, driver and allocator all affect these
figures. The older 143 MiB idle observation is described in
the [performance notes](performance.md).

## Measured session

Medians of 13 independent starts per build on 24 November 2026, with Qt Quick
rendering through Vulkan. The baseline was `66bdff57` (main), the
implementation `f40c0cb5`.

| Metric | Baseline | Implementation |
|---|---:|---:|
| Idle PSS | 101.94 MiB | 97.26 MiB |
| Idle private dirty memory | 74.86 MiB | 69.95 MiB |
| PSS after five opens and closes of Notification Center | 104.80 MiB | 99.79 MiB |
| Idle shell DRM resident VRAM | 215.21 MiB | 212.61 MiB |
| First notification-center map | 16.69 ms | 16.70 ms |
| Subsequent maps | 4.18 ms | 4.19 ms |
| Qt render-loop frame time, p95 / p99 | 0 / 1 ms | 0 / 1 ms |

The session used a Ryzen 9 9950X, amdgpu (PCI 1002:7550), CachyOS kernel
7.3.6-1-cachyos, Mesa 26.3.1 (RADV), Qt 6.12.0 and glibc 2.44. All builds used
GCC 16.2.1, CMake Release and x86-64-v3. The compositor used its headless
backend with a 1920×1080 primary output at 100 Hz and scale 1. The shell used
Qt Quick's Vulkan backend with the threaded render loop. Each run used a
private bus and a fresh profile, and the runs alternated between builds. The
timings measure when the layer maps, with 2 ms polling. The Qt render-loop
timings have millisecond resolution and include all shell windows. The VRAM
figures cover the whole shell. Individual samples:
[desktop](benchmarks/shell-memory-2026-11-24.csv),
[clipboard and notification workloads](benchmarks/shell-memory-workloads-2026-11-24.csv).

To reproduce, build `vela-shell` for each revision. Build `vela-compositor`
and `vela-screen-dump` once and use the same tools for all runs. Run from a
GPU-capable session, alternating the shell executables. The script checks
that the shell reported the Vulkan scene graph. `--workload clipboard` and
`--workload notification` run the workloads in sections 8 and 2.

```sh
VELA_BUILD=/absolute/path/to/tools-build python3 docs/benchmarks/shell-memory.py /absolute/path/to/build/shell/vela-shell
```

## 1. Clipboard images

The [clipboard](../shell/src/clipboard.cpp) keeps the full PNG payload and a
preview bounded at 544×192 pixels. Incoming PNGs retain their original bytes.
JPEG, WebP and BMP offers are encoded at their original dimensions. The async
provider reads a snapshot and uses the PNG when a larger preview is required.
A matching thumbnail triggers a full-pixel comparison before two different
encodings can be treated as duplicates.

Pinned images use the same payload. Existing numbered PNG files still load.
New pins use content-addressed PNG files. The manifest is replaced atomically
before obsolete files are removed. A Wayland round trip preserved PNG bytes
and the dimensions and decoded pixels of JPEG, WebP and BMP. The functional
test also pastes an old pin selected beyond the viewport.
[Clipboard test](../tests/functional/test_shell.py)

## 2. Notification image size

The [decoder](../shell/src/notifications.cpp) validates the D-Bus layout and
the 4096-pixel limit on each input side. It retains at most 448×448 pixels,
preserving aspect ratio. The center draws 28 logical pixels, so the same image
can serve a later output at up to 16× scale. Smaller images keep their native
size.

In three baseline and three candidate sessions, a single valid 4096×4096 RGB
notification was sent on a private bus sized for the 48 MiB message. From idle
to the center showing its history, median private dirty memory rose by
54.8 MiB on the baseline and 6.5 MiB on the candidate. This comparison includes opening
the center. The full input and decode buffer still exist during delivery.
[Image decoding and replacement tests](../shell/tests/notification_image.cpp)

## 3. Calendar hover

The [month grid](../shell/qml/NotificationCenter.qml) uses one hover handler
for all 42 days. Each day retains its text and creates a background for today,
an active hover or an outgoing fade. A quick return to a cell reverses its
existing fade. After pointer exit, only today's background remains. The
[calendar test](../tests/taskbar/tst_calendar.qml) exercises adjacent days,
re-entry and the weekday header.

## 4. glibc at startup

On glibc the [shell](../shell/src/main.cpp) selects two malloc arenas before
Qt starts, respecting an explicit arena limit in the environment. This setting
is local to the shell process. Once the initially visible Qt Quick windows
have submitted their first frames, the GUI thread calls `malloc_trim(0)` once.

Interleaved five-run trials on the baseline measured median idle private dirty
memory of 74.9 MiB with allocator defaults, 73.4 MiB with
`MALLOC_ARENA_MAX=2` alone and 74.0 MiB with a one-time experimental trim
1.2 seconds after startup. The table above measures the shipped combination
and all other shell changes together.

## 5. Desktop label shadows

Visible [desktop labels](../shell/qml/Wallpaper.qml) keep the blurred shadow.
The static eight-pixel radius uses one fewer Qt blur level. The padding
preserves the original sampling positions. Hiding a label during rename also
disables its layer. A native raised-text shadow and a cached composite both
changed the output pixels more, especially after a scale change.

Label crops were compared on light and dark wallpapers, with wrapped labels
selected and unselected, at 100%, 125%, 150%, 175%, 200%, 250% and 300%. Under
Vulkan, the captures vary between starts of the same build: two baseline
starts matched in 9 of the 28 crops and differed by up to two levels in an
8-bit channel. The baseline and the candidate matched in 7 crops. The other
crops differed by at most one level, or by two where baseline starts also
differed by two. At 300%, four starts of each build showed the same range of
differing pixels between builds as within each build. Each visible label
still has a source layer.

## 6. Tray pixmaps

Each [tray item](../shell/src/tray.cpp) owns the pixmap for its current icon.
Updating a pixmap assigns a fresh provider URL, switching to a named icon
releases the pixels, and removing the item releases its last image. The
[private-bus tray test](../shell/tests/tray_test.cpp) checks updated pixels
and the retirement of old keys at all three transitions.

## 7. Replacement notifications

When an existing notification switches from embedded pixels to a file or
theme icon, the [server](../shell/src/notifications.cpp) drops the old image
for that ID. Replacement keeps the notification and its actions addressable
both in the popup and in history. The image test checks both lifetimes.

## 8. Clipboard viewport

The [clipboard panel](../shell/qml/ClipboardPanel.qml) uses a variable-height
`ListView` ([ClipboardList](../shell/qml/ClipboardList.qml)) for visible and
nearby cards. Pin ordering and keyboard selection use the complete history,
including rows whose delegates have not been created. Each history change
replaces the model, which would scroll the view back to the top. The list
keeps the scroll position and keeps the selection on the same entry. If the
selected entry was deleted, the selection moves to the entry that takes its
place. A selection that was visible stays visible.

With 25 low-compression-cost 1600×900 RGB screenshots saved as pins, three
runs per build measured median idle PSS of 243.1 MiB before and 114.3 MiB
after. With the panel open the readings were 248.7 MiB and 116.7 MiB. This workload measures
the combined effect of image storage, the viewport and allocator changes.
