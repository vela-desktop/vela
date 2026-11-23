# Shell memory follow-ups

The code references below were checked on 23 November 2026 against `8f49a36b`.
These are proposals. The roughly 143 MiB idle PSS in the [performance notes](performance.md)
was a historical observation. Take a fresh baseline on the revision under test.

Use the same workload, icon count, outputs and scales on both builds. Verify the
shell PID for the session under test and sample `Pss` and `Private_Dirty` from
`/proc/<pid>/smaps_rollup` at the same points. For textures, also compare the
shell's per-region `drm-resident-*` counters in `/proc/<pid>/fdinfo/` when the
driver exports them. Count each DRM client once per device. A reduction in
resident GPU memory can leave PSS unchanged. Record repeated samples, frame
times and first-open latency using the [performance methodology](performance.md).
Keep effects, sharpness and next-frame opening of warm panels across all trials.

## 1. Keep a full-resolution clipboard payload and a separate preview

[`Clipboard::readOffer`, `setSelection` and `loadPinned`](../shell/src/clipboard.cpp)
retain both original bytes and a decoded image for incoming PNGs. JPEG, WebP
and BMP offers keep only decoded pixels and are encoded as PNG on paste. Pinned
images loaded at startup also start with only decoded pixels. A 2560×1440
32-bit image takes about 14 MiB before any encoded bytes.

Keep the original bytes for PNG offers. Encode other accepted images at full
resolution before discarding the decoded image, and read existing pinned PNGs
as bytes. Persist that full payload when pinning. Generate the panel preview at
the required physical size and regenerate it from the payload if the output
scale increases. Preserve `add()`'s pixel-based duplicate detection, including
re-encoded copies and distinct images with identical previews.

With history enabled, copy several large images and compare `Private_Dirty`.
Check byte-identical paste for PNG input and full dimensions and pixel equality
with the decoded source for JPEG, WebP and BMP. Repeat after pinning and
restarting, including an entry saved by the previous build.

## 2. Size notification images for the toast and the center

[`decodeNotificationImage`](../shell/src/notifications.cpp) accepts validated
8-bit RGB or RGBA data up to 4096 pixels per side. `iconFor()` keeps the entire
decoded image in `m_images`. The [toast](../shell/qml/NotificationToast.qml) and
[center](../shell/qml/NotificationList.qml) request 16 and 28 logical pixels,
respectively. Preserve aspect ratio and retain a rendition sufficient for the
largest supported output scale, including a scale change while an entry is in
history. Keep the decoder's size and buffer checks.

Send a valid 4096×4096 RGB image over the session bus and confirm the image
appears in the center before sampling retained memory. Check the toast and
the history entry at 200% and after changing scale. Run `shell-notification-image`
to check that oversized sides and invalid layouts are still rejected.

## 3. Share calendar hover handling while preserving overlapping fades

The [month grid](../shell/qml/NotificationCenter.qml) has 42 day delegates,
with a `Text`, an animated `Fill` and a hovering `MouseArea` each. Use one grid
hover handler, accounting for the weekday header. Keep a background for today
and a fill for each cell whose hover is active or still fading. Release or
reuse a fill when its fade completes. On re-entry, reverse from the cell's
current color, as [Fill](../common/controls/Fill.qml) does today.

Compare both styles at the same intermediate animation times as the pointer
moves A → B → C → A before the first fade finishes. Include pointer exit and
a pass over today. Check the outgoing and incoming cells against the baseline
and confirm the idle QML object count falls. See [visual coherence §6.8](visual-coherence.md#68-controls-qt-behaves-vela-looks-d12).

## 4. Measure glibc arenas and memory left after startup

The shell starts several threads. On glibc, freed allocations can stay in
arenas after startup. Compare the current build with `MALLOC_ARENA_MAX=2` set
for the shell process. In a separate experimental build, call `malloc_trim(0)`
once after the initially visible surfaces have drawn. Sample after the same
startup interval, then open panels and compare frame times and `Private_Dirty`
with the baseline. Check the [allocation policy](memory-allocation-policy.md)
before adopting either change.

## 5. Measure the GPU cost of desktop label shadows

Each desktop icon [label](../shell/qml/Wallpaper.qml) enables a Qt Quick layer
for its `MultiEffect` drop shadow. Try rendering the shadow without a permanent
per-label texture. Compare resident GPU bytes and frame times on identical
desktops with zero icons and with a populated grid, at each tested scale. Also
record PSS. Check cropped label pixels against the current renderer on light
and dark wallpapers, including wrapped and selected labels. A scene-graph trace
can confirm the layer count, while the memory counters establish the saving.

## 6. Release superseded tray pixmaps

[`TrayModel::refresh`](../shell/src/tray.cpp) inserts a new serial-keyed image
into `m_pixmaps` on each pixmap-backed refresh. Replacing the icon, switching
to a named icon and removing the tray item all leave the previous image in the
map. Even one long-lived item can accumulate images over the session.

Tie each retained pixmap to its live tray item and retire the previous key when
the displayed icon changes. Keep a fresh image URL on replacement so QML picks
up the new pixels. Extend the [tray test](../shell/tests/tray_test.cpp) with a
pixmap-only item that changes its icon, switches to a named icon and disconnects.
Check the displayed pixels and that obsolete keys stop resolving. With a fixed
number of items, compare retained bytes over successive batches of updates.

## 7. Drop notification pixels when a replacement uses another icon source

[`NotificationServer::iconFor`](../shell/src/notifications.cpp) replaces the
stored image when a new `image-data` hint arrives. If the same ID switches to
`image-path`, a named icon or the default, the model changes the URL but the old
pixels remain in `m_images` until final closure. This also affects replacements
of entries already in history.

Release the ID's old image when choosing a non-provider URL. Replace a live
notification and a history entry in turn. Check the new icon, unchanged ID and
a null `image(id)` for both. Test replacing it again with fresh image data.
Measure before and after replacement separately from the resizing trial in §2.

## 8. Create clipboard cards for the viewport

The [clipboard panel](../shell/qml/ClipboardPanel.qml) uses a `Repeater` inside
a `Flickable` capped at 420 logical pixels high. It creates a card for every
entry, including every pin. Most cards in a long history are outside the view.

Try a `ListView` with visible and nearby delegates, keeping the panel itself
warm. Compare QML object count, PSS, first-open and scroll frame times with the
same mix of text and image entries, first with a short list and then with many
pins. Check variable card heights, keyboard navigation across viewport edges,
paste by ID, pin reordering, deletion and the menu anchor after scrolling.
