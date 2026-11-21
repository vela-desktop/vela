# Shell memory: follow-ups

> **Status:** Proposed. **Checked:** 17 November 2026, `d784004`.
>
> **Remaining:** all five items below. None is implemented.

`vela-shell` idles at around 143 MiB PSS ([performance](performance.md)). This
page lists five places where the shell keeps memory that the screen does not
need. None of them removes an effect, a panel's warm QML tree or the
next-frame opening of a panel. Everything already in place (hidden panels
give back their surface, the wallpaper image is uncached and sized to the
output, one QML engine, level meters only while visible) is out of scope.

Each change is measured before and after with `Pss` and `Private_Dirty` from
`/proc/$(pidof vela-shell)/smaps_rollup` under the same workload. A change that
does not show up there is not merged as a memory improvement.

## 1. Clipboard history keeps two full copies of each image

`Clipboard::Item` (`shell/src/clipboard.h`) holds both the decoded `QImage`
and the `png` bytes for every image entry, up to 25 unpinned entries plus the
pinned ones. A 2560×1440 screenshot decodes to about 14 MiB, on top of its
PNG. The panel only draws a thumbnail (`ClipboardImageProvider` scales the
full image at every request).

Change: keep the PNG (it is what goes back on the clipboard) and a thumbnail
at the largest size the panel draws, made once when the entry arrives. Decode
the PNG only where a full image is actually needed.

Verify: copy several large images, compare `Private_Dirty`; existing clipboard
tests still pass; pasting an entry returns the identical PNG.

## 2. Notifications keep `image-data` at full resolution

`NotificationServer::iconFor` (`shell/src/notifications.cpp`) stores the
decoded `image-data` hint as it arrived in `m_images`, and
`NotificationImageProvider` scales it at every request. The size is bounded
only by the D-Bus message.

Change: scale once on arrival to the largest size a toast or the center
shows, at the highest output scale, and store that.

Verify: a test sends a large `image-data` notification over D-Bus; compare
memory; the image is still sharp at 200% scale.

## 3. The calendar builds a control per day

The month grid in `shell/qml/NotificationCenter.qml` is a `Repeater` of 42
delegates, each with its own `Fill` (with its `Behavior`/`ColorFade`), `Text`
and hovering `MouseArea`. Only one cell can be under the pointer and one can
be today.

Change: keep 42 `Text` items; one `HoverHandler` on the grid computes the cell
under the pointer; one shared hover `Fill` and one today `Fill` move to their
cell. The fade timing must stay identical
([visual coherence §6.8](visual-coherence.md#68-controls-qt-behaves-vela-looks-d12)).

Verify: before/after screenshots of hover and today in both styles match; the
Notification Center's object count drops (`qmlprofiler`).

## 4. glibc arenas and memory left after startup

The shell starts several threads (QML, render, D-Bus, PipeWire, Mesa). glibc
may create up to eight arenas per core, and memory freed in them after
startup is not necessarily returned to the kernel. `vela-shell` currently
neither limits arenas nor trims.

Change, measured first without code: run the shell with
`MALLOC_ARENA_MAX=2`, and separately call `malloc_trim(0)` once after the
first frame of every surface. Keep only what shows a real `Private_Dirty`
saving and no frame-time regression (`docs/performance.md` method). The
shell is outside `scripts/check-allocations.py`'s scope, but any change must
stay consistent with [the allocation policy](memory-allocation-policy.md).

## 5. A permanent offscreen layer per desktop icon label

In `shell/qml/Wallpaper.qml` every desktop icon's label has
`layer.enabled: true` with a `MultiEffect` drop shadow: one offscreen
texture and one extra render pass per icon, for the whole session. (The other
layers in `common/controls`, `RoundClip` and `StartIcon`, are already
conditional.)

Change: draw the shadow without a per-item layer, for example one effect over
the whole icon container, or a shadow text drawn behind the label, keeping
the same look.

Verify: desktop pixel tests at every scale; `QSG_INFO=1` and `qmlprofiler` show
the layers gone.
