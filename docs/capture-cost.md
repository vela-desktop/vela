# Lower-cost screen capture

Intel GPU [run #5](https://github.com/vela-desktop/vela/actions/runs/43619532573)
completed all captures but failed SHM throughput and presentation budgets.
Capture is inherently work: the first choice is which consumers actually need
CPU pixels. The existing SHM checks remain unchanged and must still pass on
the exact Intel candidate before declaring that failure resolved.

## Paths already available

| Consumer | Existing path | Cost to avoid |
| --- | --- | --- |
| Screen sharing and ordinary encoded recording | DMA-BUF capture into GPU buffers, then GPU consumption/encoding | GPU-to-host staging, client-SHM writes and CPU-to-GPU uploads |
| PNG screenshots, raw recording evidence and pixel tests | SHM capture | CPU pixels are part of the output contract; keep the fallback correct and responsive |
| Shell window previews | Currently SHM followed by a QImage copy | Full-resolution readback and another CPU image before a smaller preview is displayed |

`scene/capture.c` already detects DMA-BUF destinations and delegates to
wlroots' GPU-to-GPU copy helper. There is no need to introduce another capture
protocol to use it. This is not a claim of zero GPU copies: the compositor may
still blit, convert and synchronize the destination.

The existing xdg-desktop-portal-wlr backend can deliver DMA-BUF video through
PipeWire. Its
[producer implementation](https://github.com/emersion/xdg-desktop-portal-wlr/blob/master/src/screencast/pipewire_screencast.c)
and PipeWire's [DMA-BUF negotiation contract](https://docs.pipewire.org/devel/page_dma_buf.html)
provide the reusable route for compatible sharing/recording consumers. The
producer and consumer must agree on a format/modifier; unsupported negotiation
needs a working SHM fallback. Confirm the negotiated buffer type in the portal
logs rather than assuming that selecting a hardware encoder avoids readback.

For ordinary recording, wf-recorder already tries GPU buffers by default and
supports VA-API encoding. Its
[upstream manual](https://github.com/ammen99/wf-recorder/blob/master/manpage/wf-recorder.1)
documents, for example:

```sh
wf-recorder -f recording.mkv -c h264_vaapi -d /dev/dri/renderD128
```

Choose the actual compatible render node; `renderD128` is an example. Keep
DMA-BUF enabled and verify that the encoder/driver supports the selected
format. This is an alternative for encoded video, not a replacement for Vela's
raw recording evidence or its pixel/latency tests. Those outputs and schedules
must retain their existing semantics.

## Immediate SHM improvement

`render/texture.c` previously copied a row with `memcpy`, then read and wrote
the destination again to force opaque alpha for XRGB sources captured into
ARGB buffers. The copy now reads each source pixel and writes its final opaque
value in one pass. Packed 8-bit and 10-bit formats preserve every color bit;
half-float pixels copy RGB and write alpha 1.0 directly. Formats needing no
alpha conversion still use `memcpy`.

The staging and destination mappings are disjoint. Small `memcpy` word accesses
preserve unaligned-row and strict-aliasing correctness while allowing compiler
vectorization. Regression coverage spans every source/destination byte
alignment, vector-loop tails, untouched padding and all alpha policies.

This removes a redundant memory pass. It does not remove the GPU copy, first
write page faults, event-loop work or presentation contention, and does not
establish a particular Intel frame rate. Existing cached staging, bounded
deferred slices, shared GPU readbacks and persistent recorder buffers already
exist; adding them again would not address this failure.

## Next changes to qualify independently

1. **Use damage when buffers are reused.** The
   [capture protocol](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/main/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml)
   permits copying the union of source damage and client buffer damage.
   Vela's SHM renderer currently copies the full image; the recorder and probe
   also mark the entire destination damaged. Track damage separately for each
   reused buffer, including changes while another capture is pending, and pass
   the correct accumulated union to a bounded regional GPU/CPU copy. Audit the
   wlroots source contract before doing this; do not read private session
   fields or mistake one output event's damage for all changes since a
   particular buffer was captured. First captures, new buffers and changed
   constraints need a complete image. Cancellation, lock and source removal
   must preserve existing ownership guarantees.
2. **Reduce preview pixels before readback.** Render a window preview at its
   requested size, or import a DMA-BUF into Qt's graphics backend with explicit
   buffer ownership and synchronization. `capture/` currently exposes only
   CPU SHM frames, and `shell/src/windowcapture.cpp` makes an owned QImage copy;
   neither is a GPU-native preview API. Keep full-size screenshots separate.
3. **Measure CPU-copy scheduling against presentation deadlines.** Compare
   the same animation without capture, then SHM with fresh/reused destinations
   and DMA-BUF. Record GPU completion, CPU slice duration, page faults and late
   wakeups. Retain the 180 Hz workload, frame counts and presentation limits;
   a smaller CPU slice alone can trade throughput for responsiveness.

`VK_EXT_host_image_copy` is another experiment, not a guaranteed faster path.
The [Vulkan proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_host_image_copy.html)
requires feature, format, usage/layout and performance checks. An imported
output image cannot simply be treated as an ordinary CPU pixel array. Prefer
the existing DMA-BUF route when the consumer can keep pixels on the GPU.

Accept a follow-up only with pixel/damage/ownership tests and repeated hardware
comparisons. Optimizing the full-frame SHM path and offering a cheaper GPU
consumer path are separate results; neither permits skipping the other.
