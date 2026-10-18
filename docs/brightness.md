# Brightness

Design of display brightness in Vela: a slider per monitor, in quick settings
and in Settings → Display, that changes the monitor's own backlight. External
monitors are driven over DDC/CI, spoken by Vela itself; the built-in screen
of a laptop through its kernel backlight. The compositor owns the state of
every output. This document comes before the code: decisions are made here,
and the code follows them.

> **Status:** design approved; B0 done, its timings measured with `--bench`
> on the monitors of §3; B1 to B4 done, tested on fake monitors and a
> fake backlight; the tests on real hardware of §12 remain (§15).

## 1. Goals

1. **Real brightness.** The slider moves the panel's backlight, as the
   buttons on the monitor do. A dimmer monitor uses less power.
2. **One model, per output.** Every output, external or built-in, has its
   own brightness, owned by the compositor. This is also where the HDR work
   (renderer S7) will need it (§10).
3. **Immediate.** A slider appears as soon as its monitor does, and the
   monitor follows a drag while it happens, not in steps twice a second.
4. **Honest.** A slider appears only for a monitor that answers. When a
   monitor drops a write (both test monitors do, §3), Vela notices and
   writes again. What the slider shows is what the monitor has, including
   changes made with the monitor's own buttons.
5. **Never in the way.** A slow, broken or hung monitor never delays a frame,
   never delays another monitor, never blocks the shell, and never holds a
   device the kernel needs back (§4.3).
6. **Learn from the others.** KDE Plasma, GNOME and COSMIC have met most of
   the problems already (§4); Vela starts from their fixes.
7. **Tested without hardware.** The automated tests run on fake monitors that
   reproduce, byte for byte, the faults measured on real ones, and the bugs
   others have fixed (§12).

## 2. Decisions taken

| Decision | Choice | Consequence |
|---|---|---|
| Mechanism | **The monitor's backlight only**: DDC/CI (VCP `0x10`) for external monitors, the kernel backlight for built-in screens | no software dimming in SDR, not even as a fallback (§11): dimming the signal leaves the backlight as bright as before, so the screen loses contrast and levels without saving power |
| Who owns the state | **the compositor**, for every output | the same model for DDC/CI, the built-in backlight and later HDR (§10); the shell and Settings are only clients (§9) |
| Where the hardware is touched | **`vela-brightness`**, a small C helper, **one process per output**, started by the compositor | slow I²C and D-Bus calls stay out of the compositor; a hung monitor is one process, killed alone (§6) |
| DDC/CI | **Vela's own**, straight on `/dev/i2c-N` | no libddcutil: no 2–2.5 s start-up scanning every bus, no global state, timings measured and chosen by us (§3, §5.2) |
| Built-in backlight | **sysfs to read, logind `SetBrightness` to write** | no root; moves from the shell into the helper (§5.3) |
| Monitors without brightness | **no slider**; Settings says why (§8) | a monitor that answered and then stops shows a **locked** slider |
| Writes while dragging | **the latest value wins**, at most one DDC/CI write every **250 ms**, then a **read back** 500 ms after the slider rests | smooth slider, no lost final value (§5.4) |
| Live sync | **while a brightness slider is on screen**, its outputs are read **once a second** | the monitor's own buttons move the slider (§5.5) |
| What Vela remembers | **nothing**: no saved brightness, no cache of monitors or timings | the monitor stores its brightness; Vela reads it and never imposes a value |
| All monitors together | **no** | each slider is one output |
| Power policy | **not now**: no idle dimming, battery profiles or ambient light | a future policy service would be one more client of the model (§10) |
| Keys and OSD | **later** | `XF86MonBrightnessUp/Down` will act on the monitor under the pointer, with an OSD like the volume one (§10) |

## 3. Measurements

Taken on 2026-10-13 on the main development machine (RX 9070 XT, amdgpu,
[testing](testing.md#primary-development-hardware)), both monitors at
0–100, every test ending with both back at 100.

| | LG FULL HD (HDMI-A-1) | Mi Monitor (DP-3) |
|---|---|---|
| Bus | `i2c-6` ("AMDGPU DM i2c hw bus 3") | `i2c-9` ("AMDGPU DM aux hw bus 2", DisplayPort AUX) |
| libddcutil: `ddca_init2` (once, scans every bus) | 2–2.5 s | |
| libddcutil: read / write | ~105 ms / ~51 ms | ~115 ms / ~57 ms |
| Vela's prototype: read | **42 ms** | **56–160 ms** (retries included) |
| Vela's prototype: write | < 1 ms (the bus returns, the monitor works on it) | 7–11 ms |

The prototype used `read()`/`write()` on the device. Measured again on
2026-10-17 with `vela-brightness --probe`, which uses the `I2C_RDWR`
`ioctl()` (§5.2): a read of the LG takes 42 ms in one attempt, a read of the
Mi 150–260 ms, because its first one or two replies are the fixed block
below. Opening the device, taking the lock and closing it costs about 1 µs.

**The LG ignores a write that follows another by 5 to 50 ms.** Two writes,
90 then 70, with a pause between them, final value read 500 ms later (two
rounds each):

| Pause (ms) | 0 | 5 | 10 | 20 | 50 | 100 | 150 | 200 | 300 | 500 | 800 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| LG | 70 70 | 90 90 | 90 90 | 90 90 | 90 90 | 70 70 | 70 70 | 70 70 | 70 70 | 70 70 | 70 70 |
| Mi | 70 70 | 70 90 | 90 90 | 70 70 | 90 70 | ✗ 70 | 90 ✗ | 70 70 | 70 70 | 100 70 | 70 100 |

(✗: the read failed four attempts in a row; 100: neither write was kept.)

Measured again on 2026-10-18 with `vela-brightness --bench` (§11), which
writes with the `ioctl()` of §5.2: 90 then 80, final value read 1.5 s later,
three rounds each, then a second run around the edge:

| Pause (ms) | 0 | 20 | 50 | 100 | 150 | 160 | 170 | 180 | 190 | 200 | 250 | 300 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| LG | 80 80 80 | 90 90 90 | 90 90 90 | 90 90 90 | 90 90 90, 80 80 80 | 90 90 90 | 90 90 80 | 90 90 90 | 80 80 90 | 80 80 80 ×2 | 80 80 80 | 80 80 80 |
| Mi | 80 90 90 | 80 80 80 | 80 80 80 | 80 90 80 | 90 80 90 | | | | | 80 90 80 | | |

So the LG ignores a write that comes up to about 190 ms after another, and
keeps every one from 200 ms. That is four times the 50 ms DDC/CI asks for,
and twice what the read()/write() prototype suggested. It also explains the
libddcutil drag (a write every ~51 ms) that left the LG at 95 instead of 100:
most writes were ignored, the last one included. The Mi drops writes at
random at any pause. **No fixed interval is enough: only a read back tells.**

In a second run of the Mi, reads after two writes less than 180 ms apart
failed all six attempts 6 times in 21; from 180 ms, never in 15. Writing too
close together costs the Mi its replies too.

**A read right after a write tells the truth.** A write, a pause, a read,
four rounds per pause from 50 to 1000 ms: the LG returned the new value in
28 reads of 28 (two attempts up to 100 ms, one from 200 ms); the Mi returned
the old value now and then at any pause, from 100 to 750 ms, as a dropped
write does. Then one write followed by a read at 50, 300, 500, 750, 1000,
1500, 2000 and 3000 ms, eight rounds on the Mi: in seven the new value was
there from 50 ms to the end, in one the old value from 50 ms to the end.
The Mi never applied a write and then undid it, so whatever the first read
says stays.

**Half of the Mi's replies are not replies.** About 10 in 20 reads return a
block of bytes that is always the same
(`2f 9a 2d 3a b5 ff 9d 37 b3 63 23`), or
fail with an I/O error, whatever the wait before reading (40, 60, 80 or
120 ms). The checksum rejects them every time; the next attempt gets the real
reply (`6e 88 02 00 10 00 00 64 00 64 a4`). The LG returned 2 bad replies
in 160.

Neither monitor ever reported an error for a write it ignored.

## 4. What the others do

Checked on 2026-10-13. Each point links to its source.

### 4.1 KDE Plasma

DDC/CI lives in **PowerDevil**, the power daemon, with libddcutil in a thread
of its own (master at
[`a577bde5`](https://invent.kde.org/plasma/powerdevil/-/blob/a577bde5/daemon/controllers/ddcutildisplay.cpp),
2026-07-06, "Make ddcutil monitor brightness control more responsive"; latest
tag v6.7.91). PowerDevil hands the monitors to KWin through
[`kde-external-brightness-v1`](https://wayland.app/protocols/kde-external-brightness-v1),
to keep unstable mechanisms, or ones that need root, out of the compositor.

- The first write 50 ms after the slider moves, then at most one every
  500 ms; **no read back**.
- Failed writes retried after 1, 2 and 3 s; then the monitor is removed from
  brightness control until PowerDevil restarts (also
  [reported by users](https://discuss.kde.org/t/brightness-slider-missing-in-plasma-when-powerdevil-has-no-ddc-or-backlight-backend/48004)).
- The display handle is opened for each operation, so `ddcutil` keeps
  working.

### 4.2 GNOME

GNOME has no DDC/CI of its own (extensions do it with `ddcutil`). Its lesson
is about ownership:
[since GNOME 49](https://blog.sebastianwick.net/posts/gnome-49-backlight-changes/)
the backlight moved from gnome-settings-daemon into **Mutter**, "the single
source of truth for the backlight itself":

- the old code pretended there was one internal display; now any screen can
  have a backlight, and quick settings controls several;
- in HDR, the backlight, the reference white and the headroom above it are
  one problem, and Mutter dims HDR in the signal ("emulates a backlight in
  software"), which 10- and 12-bit signals allow without banding;
- KMS will get a backlight API driven by Mutter, so the backlight can change
  atomically with a frame;
- a backlight value of 0 can mean "the display turns off or is just really
  dim".

### 4.3 COSMIC

`cosmic-settings-daemon` drives DDC/CI with the `ddc-hi` crate in one thread
([`brightness_device.rs`](https://github.com/pop-os/cosmic-settings-daemon/blob/67c995d5/src/brightness_device.rs),
2026-08-28). Its design is simpler than Vela's: **one** target for every
monitor, the maximum assumed to be 100, the starting value assumed to be 100
and never read, every display enumerated and its capabilities read on every
write, no read back. Its bugs are the most useful part:

| Bug | What happened | Fix |
|---|---|---|
| [#165](https://github.com/pop-os/cosmic-settings-daemon/issues/165) | `/dev/i2c-*` held open forever. Unplugging a DP-MST monitor makes the kernel wait in `i2c_del_adapter` for every open fd: a kworker hung in D state, hotplug stalled desktop-wide (Wayland clients up to 21 s), stale connectors, so the monitor came back as DP-7, DP-8, DP-11 and lost its saved layout | [#171](https://github.com/pop-os/cosmic-settings-daemon/pull/171): drop the handles after 2 s without changes |
| [#183](https://github.com/pop-os/cosmic-settings-daemon/issues/183) | every read of the brightness was a DDC/CI read; during a write it was slow or failed (error 121), the slider held wrong values | [#184](https://github.com/pop-os/cosmic-settings-daemon/pull/184): return the requested value while a write is pending |
| [cosmic-applets #1483](https://github.com/pop-os/cosmic-applets/issues/1483) | capabilities read before every write: ~2.57 s per value instead of ~53 ms; enumeration ~2.8 s; one slider moved one monitor or the other at random | — |
| [#150](https://github.com/pop-os/cosmic-settings-daemon/issues/150) (open) | one brightness for every display: slow, surprising changes on multi-monitor setups | — |

COSMIC also guards the built-in backlight: it never writes 0 unless the
panel has 20 levels or fewer, and when `actual_brightness` reads 0 after a low
non-zero write (OLED panels) it raises the value by one, up to three times.

### 4.4 What Vela takes, and where it goes further

| | Lesson | Vela |
|---|---|---|
| GNOME | the compositor owns the backlight, per output, with HDR in mind | the compositor owns every output's brightness (§7); S7 builds on it (§10) |
| COSMIC #165 | an idle daemon must hold no I²C device | the device is opened for one transaction and closed right after (§5.2); unplug ends the output's process (§6) |
| COSMIC #183/#184 | never read while a write is pending; show the requested value | `target`, `confirmed` and `pending` kept apart (§7.1); live reads never overlap writes (§5.5) |
| COSMIC #1483 | no enumeration or capabilities on the hot path | the compositor names the connector, the helper opens that bus and touches only VCP `0x10` |
| COSMIC #150 | one target for several monitors doesn't work | one target, one process per output |
| COSMIC backlight | 0 can switch a panel off | the built-in minimum and the OLED guard (§5.3) |
| KDE | DDC/CI outside the compositor | `vela-brightness` (§6) |
| KDE | (no read back, 500 ms steps, permanent removal) | read back, 250 ms, `not-responding` recovers by itself (§5.4, §7.1) |

## 5. `vela-brightness`

```
 vela-shell        vela-settings
      \                 /
   command socket ($XDG_RUNTIME_DIR/vela-<display>.sock)
                |
         vela-compositor  ── outputs, hotplug, power, session, watchdog
          /     |      \
   socketpair per output (lines)
        /       |        \
 vela-brightness   vela-brightness   vela-brightness
 ddc card1-DP-3    ddc card1-HDMI-A-1   backlight amdgpu_bl1 (laptop)
       |                 |                  |
   /dev/i2c-9        /dev/i2c-6      sysfs + logind (D-Bus)
```

- Written in C, like the compositor, in `brightness/`: the DDC/CI protocol
  (pure, §5.2), the scheduler (pure, §5.4), the two backends and a fake one
  (§12).
- **One process per output, one thread per process.** A process handles one
  monitor and nothing else, so there is nothing to share and no locks.
  New lines from the compositor are read between transactions; when several
  `set` lines are waiting, only the last one counts.
- The process lives as long as its output (§6).

### 5.1 Finding the device

The compositor names what to drive:

- **DDC/CI:** the DRM connector (`card1-DP-3`). The helper looks in
  `/sys/class/drm/<connector>/` for an `i2c-N` directory (the DisplayPort
  AUX channel: DP-3 → `i2c-9`), otherwise the `ddc` link (HDMI-A-1 →
  `i2c-6`). For DisplayPort the `ddc` link points at a bus that doesn't reach
  the monitor (DP-3's `ddc` is `i2c-5`), so AUX comes first.
- **Backlight** (`compositor/src/backlight_choice.c`): a device in
  `/sys/class/backlight` whose parent is the connector itself is that
  output's (i915's `intel_backlight` hangs under `card1-eDP-1`). Otherwise a
  built-in output (`eDP-*`, `LVDS-*`, `DSI-*`) takes one whose parent is its
  DRM card or the card's PCI device (amdgpu's `amdgpu_bl1`), then any; each
  time by `type` in the usual order (`firmware`, `platform`, `raw`), then by
  name. An internal output without a backlight device tries DDC/CI, like an
  external one. The user's own machine is a desktop: laptops are checked in
  B4 (§12).

### 5.2 DDC/CI

Only what brightness needs, from the DDC/CI and MCCS standards:

| Message | Bytes to address `0x37` | Reply |
|---|---|---|
| Get VCP | `51 82 01 10 <xor>` | after a wait, 11 bytes: `6e 88 02 <result> 10 <type> <max hi> <max lo> <cur hi> <cur lo> <xor>` |
| Set VCP | `51 84 03 10 <hi> <lo> <xor>` | none |

- **Checksums:** the request's is the XOR of `0x6e` and its bytes; the
  reply's is the XOR of `0x50` and its first 10 bytes. A reply is accepted
  only if source (`0x6e`), length (`0x88`), opcode (`0x02`), feature (`0x10`)
  and checksum all match. The Mi's fixed block, a null message (`6e 80 be`,
  "not ready") and I/O errors are all a failed attempt. The parser is a pure
  function, unit-tested on the bytes captured in §3.
- **Transfers:** each message is one `I2C_RDWR` `ioctl()` with the address
  in the message, so a kernel driver bound to `0x37` (the out-of-tree `ddcci`
  module) can't make the device `EBUSY`. If the driver rejects it with
  `EINVAL`, the helper switches to `I2C_SLAVE` and `read()`/`write()` for
  that device. This is ddcutil's strategy since 1.4: some builds of NVIDIA's
  proprietary driver reject the `ioctl()` path
  ([ddcutil #282](https://github.com/rockowitz/ddcutil/issues/282)).
- **Reads:** up to **6 attempts**, 50 ms apart, with a **40 ms** wait between
  request and reply (the standard's). With 4 attempts a read of the Mi still
  failed now and then (§3).
- **The device is held only for a transaction**: open, `flock()`, one read or
  one write, unlock, close. Nothing is open while the helper waits, between
  the writes of a drag, or between live reads (COSMIC #165, §4.3). Opening,
  locking and closing `/dev/i2c-N` costs about 1 µs (§3).
- **`flock()` is non-blocking**, retried every 50 ms for up to 1 s: another
  program (`ddcutil` from a terminal) is using the bus. That is `busy`, not
  a failure: a live read is skipped, a write waits within its budget (§5.4).
  ddcutil 3.0 takes the same lock on the same device by default (its
  "cross-instance locking"): with the bus held, it retried 32 times in 3 s
  and gave up with `DDCRC_FLOCKED`. So Vela and ddcutil never talk over each
  other.

### 5.3 Built-in backlight

- **Read:** `brightness` and `max_brightness` in sysfs (readable by anyone),
  and `actual_brightness` when present.
- **Write:** logind's `SetBrightness("backlight", <device>, <value>)` on the
  session (`/org/freedesktop/login1/session/auto`: the helper is in the
  compositor's session), over D-Bus (`sd-bus`), with a 2 s timeout. No root.
- **Minimum:** never 0 when the device has more than 20 levels (a 0 can turn
  the panel off, §4.2); 0 is allowed on coarse devices, where it is a level.
- **OLED guard:** after a write at or below 5% of the maximum, if
  `actual_brightness` reads 0, raise the value by one, up to three times
  (COSMIC, §4.3).
- No coalescing is needed (a write takes a D-Bus round trip): the scheduler
  of §5.4 runs with no interval between writes and a read back after
  100 ms. The same lines, states and live sync apply: some laptops change the
  backlight in firmware, behind Vela's back.
- The minimum and the OLED guard can make the device's value differ from
  the target: the helper then confirms what it applied, so a slider at 0
  ends at 1.
- Values are 32-bit everywhere (lines, scheduler, model): `max_brightness`
  passes 65535 on some Intel panels.

### 5.4 Writing a value (DDC/CI)

Every `set` from the compositor carries a sequence number (§5.6). The
helper:

1. waits for a target different from the confirmed value;
2. writes the target, unless the previous write is less than **250 ms** old
   (the LG's 200 ms with a margin, §3); then it waits for the rest of the
   interval;
3. after each write, takes the newest target and goes back to 2 if it
   changed (a drag gives four writes a second);
4. when no new target came for **500 ms** after the last write, reads the
   value back. A different value means the write was dropped: write again
   (step 2). Both monitors of §3 answer the truth 50 ms after a write, and
   it doesn't change later; the pause, two write intervals, tells a slider
   at rest from a drag that only slows down, so a drag doesn't pay for read
   backs between its writes;
5. reports the confirmed value with the sequence number of the target it
   confirms.

**Budget:** a target gets at most **4 rewrites and 8 s**, whichever comes
first (with the 500 ms pause, a rewrite costs about 1 s on the Mi). Then the helper
reports the value it read as `refused` (§7.1): the monitor locks its
brightness (HDR, some picture modes) or keeps losing writes. If reads keep
failing, the output is `not-responding`.

**The user comes first.** A new target always replaces the old one: a read
back still running finishes (a transaction can't be interrupted), but its
result only confirms the target it was started for. Live reads (§5.5) happen
only when nothing is pending.

The numbers are a starting point, tuned in B0 against the two monitors of §3
and kept in one place. There is no adaptive timing: the read back already
guarantees the final value, and a too short interval during a drag only
skips some intermediate values.

### 5.5 Reading

- **Probe**, when the process starts: one read (value and maximum). The
  result is one of the states in §7.1: `ready`, `unsupported` (the monitor
  answered: VCP `0x10` unsupported, or a maximum of 0), `no-answer`,
  `no-access`, `no-bus`. `no-answer` is not proof that the monitor lacks
  DDC/CI (it may be waking up), so it is probed again after 2, 10 and 30 s,
  then left alone until something changes (§7.1).
- **Live sync:** while at least one client shows a brightness slider (§7.2),
  the output is read **once a second**, never while a write is pending and
  not within 1 s of the last one (the read back covers that second, §5.4).
  A good reply with a different value updates the model, so the slider follows
  the monitor's buttons. Bad replies never move a slider.
- **Wake-up:** when the compositor turns the output back on, or the session
  becomes active again, one read after 2 s.
- **System suspend:** the compositor doesn't know when the computer sleeps
  (only the shell listens to logind's `PrepareForSleep`, to lock the
  screen), and monitors take a few seconds to answer DDC/CI after resume.
  The helper notices on its own: `CLOCK_BOOTTIME` counts the time spent
  suspended and `CLOCK_MONOTONIC` doesn't, so a growing difference between
  them means the computer slept. It then behaves as on wake-up: nothing for
  2 s, then one read, and failures in that window count neither towards
  `not-responding` nor towards a write's budget. No D-Bus in the compositor
  for this.

### 5.6 Lines between the compositor and `vela-brightness`

The process is started as `vela-brightness ddc <connector>` or
`vela-brightness backlight <device>`.

Compositor → helper:

| Line | Meaning |
|---|---|
| `set <seq> <raw>` | a new target, in the device's units (0 to max); `seq` grows with every target of this output |
| `live on\|off` | start or stop live sync |
| `power on\|off` | the compositor turned the monitor off or on (idle, `test-power`); no traffic while off |
| `session active\|inactive` | VT switch: no traffic while another session owns the screens |
| `probe` | probe again (Settings is showing this output, §8) |

Helper → compositor:

| Line | Meaning |
|---|---|
| `state <state>` | `ready <raw> <max>`, `unsupported`, `no-answer`, `no-access`, `no-bus`, `not-responding` |
| `value <seq> <raw>` | a confirmed value: for target `seq` after writes, or from a live read (the last `seq` it has seen) |
| `refused <seq> <raw>` | the budget ran out: the monitor kept `<raw>` |
| `busy` / `idle` | a transaction started or ended (for the watchdog, §6) |

## 6. The helper's life

- **One `vela_child` per output** (`process.h`). The compositor starts it
  when an output appears (DRM outputs only, unless the fake backend is in
  use), and stops it when the output is unplugged or disabled: the process
  exits and nothing stays open on the device. The compositor closes its end
  of the socket and sends `SIGTERM`, and `SIGKILL` after 5 s if it is still
  there. An output turned off for idle keeps its process (`power off`).
- It talks to the compositor on a socketpair it inherits (the fd number in
  `VELA_BRIGHTNESS_FD`): `vela_child` keeps that fd across `exec`
  (`keep_fd`), and leaves relaunching to the brightness code (`exited`),
  since every process gets a new socket.
- **Watchdog, per output:** a transaction `busy` for more than **5 s** is a
  hung transfer. The compositor kills that process only; the output becomes
  `not-responding`; the other monitors don't notice.
- **No restart loops:** a process killed by the watchdog, or one that
  exits three times in a row within 5 s of starting, is not relaunched until
  the output is plugged in again, turned back on (idle or Settings), the
  session becomes active again, or Settings probes it (`brightness-probe`).
  While an old process still exists, no new one is started for that
  connector.
- **What a process can't fix:** a thread stuck in the kernel in D state
  doesn't die on `SIGKILL` until the kernel lets go. The compositor never
  waits for it: the output stays `not-responding`, and Vela holds no other
  device open that would make it worse (§5.2).
- After `vela-brightness` crashes and is relaunched, the output goes back to
  `probing`; replies from a process that is gone are never read, so a replug
  or a restart can't apply a stale value.
- `brightness=no` in `vela.conf` starts no helper; `VELA_BRIGHTNESS`
  (another command, or `0` for none) works like `VELA_POLKIT_AGENT`. By
  default it runs only in a real (DRM) session; `VELA_BRIGHTNESS_FAKE` (§12)
  starts it in any session, for every output, without touching hardware.
- Access to `/dev/i2c-*` needs no root: a udev rule gives the session's user
  `uaccess` on the buses of display adapters, and `i2c-dev` is loaded at boot
  (§13).

## 7. The compositor

### 7.1 The model

Each output has:

| Field | |
|---|---|
| `state` | below |
| `target`, `seq` | the latest value asked for, and its number |
| `confirmed` | the latest value read from the monitor |
| `pending` | `target` not yet confirmed |
| `refused` | the latest target was refused; cleared by the next change |
| `max` | the device's maximum |
| `mechanism` | `ddc` or `backlight` (S7 may add one, §10) |

| State | Meaning | Slider |
|---|---|---|
| `probing` | the helper is starting or probing | hidden |
| `ready` | brightness works | shown |
| `unsupported` | the monitor answered, but has no brightness to change | hidden |
| `no-answer` | nothing answers on the bus: DDC/CI may be off in the monitor's menu | hidden |
| `no-access` | the device can't be opened (the udev rule is missing) | hidden |
| `no-bus` | the connector has no I²C bus | hidden |
| `not-responding` | was `ready`, reads keep failing, or the watchdog fired | shown, locked |
| `off` | the compositor turned the monitor off | hidden |

- **What clients show** is `target` while `pending`, `confirmed` otherwise,
  so the slider never jumps back while the monitor catches up (COSMIC #184).
- **Sequence numbers:** `value <seq>` with a `seq` older than the current
  one updates `confirmed` but never `target`: a read back of 40 that arrives
  after the user moved on to 80 doesn't move the slider.
- **Recovery:** `not-responding` and `no-answer` go back to `ready` on the
  next good read (live sync, wake-up, replug, a `probe` from Settings). No
  state is permanent while the output exists.

### 7.2 Commands

New lines on the command socket (`command.h`):

| Line | |
|---|---|
| `brightness` | question: every output that has a helper, one JSON line: `{"outputs":[{"name":"DP-3","description":"…","mechanism":"ddc","state":"ready","value":0.8,"pending":false,"refused":false,"max":100,"seq":3}]}`; `description` is the monitor's, as Settings shows it; `mechanism` is `ddc` or `backlight`; `value` is `null` until the monitor answered; `seq` is the latest target's number (§7.1) |
| `brightness <output> <value>` | set, `value` from 0 to 1; the compositor rounds to the device's units |
| `brightness-probe <output>` | probe again (Settings, §8) |
| `watch brightness` | the connection stays open and receives the same JSON line after every change |
| `brightness-live on\|off` | this connection is showing a slider, or no longer is |

- **`watch` is new, and generic:** today the compositor pushes lines only
  to the shell (`vela_shell_send`), and Settings asks questions. `watch
  <topic>` is built in B1 as a mechanism of the command socket, not of
  brightness: a topic is a name and a function that writes its whole state as
  one line, and brightness is the first topic. The shell and Settings both
  use `watch brightness`, so they get the same updates through the same
  code; later topics (outputs, night light) reuse it.
- **A slow watcher can't block the compositor:** every line is the whole
  state, so for each watcher the compositor keeps only the latest line not
  yet written, writes without blocking, and replaces it when a newer one
  comes. A client that stops reading misses intermediate states, never the
  last one.
- **Live sync is counted per connection:** on while any connection has it
  on; a connection that closes counts as `off`. A Settings window that
  crashes doesn't leave the monitors being read forever.

## 8. What the user sees

- **Sliders** are the shared `Slider` of Vela.Controls
  (`common/controls/Slider.qml`), in quick settings and in Settings.
- **Quick settings:** one slider, for the output the panel opens on. If that
  output has no brightness, no slider. With more than one output with
  brightness, the slider has an arrow (like the volume) that opens a
  **Brightness** page with a slider per output, named as in Settings →
  Display. The wheel moves it by the same step as today. Live sync while the
  panel is open.
- **Settings → Display:** the "Brightness" card ("Brightness & color"
  section, `settings/qml/BrightnessCard.qml`) follows the output selected
  in the arrangement, with live sync while the page is visible. When that
  output has no slider, the card explains why, once, without a slider:
  - `no-answer`: "Brightness is controlled by the display's buttons. If its
    menu has a DDC/CI option, turning it on lets Vela control it.";
  - `unsupported`: "This display doesn't allow changing its brightness";
  - `no-access`: "Vela can't reach this display. The installation may be
    incomplete";
  - `no-bus`, `probing`, `off`: no card.

  Showing an output probes it again (`brightness-probe`): when the page
  opens, when another output is selected, and when the Settings window
  becomes active again. So turning DDC/CI on in the monitor's menu and coming
  back to Settings is enough, and a helper held after a crash loop (§6) gets
  another chance.
- **Refused:** the slider goes back to the monitor's value and says "The
  display didn't accept the change". The text stays until the next change.
- **Not responding:** the slider is locked and says "This display isn't
  responding". It unlocks by itself.

## 9. Shell and Settings

- A **`Brightness`** model in `shell/src/brightness.{h,cpp}`, compiled into
  both (as `SystemStatus` is today), and only a client: it keeps a
  `watch brightness` connection and exposes per output `available`,
  `locked`, `pending`, `refused`, `reason`, `value` and `setValue()` to QML,
  plus `live` for the views that show a slider.
- The backlight code leaves `SystemStatus` (`brightnessAvailable`,
  `brightness`, `setBrightness`): the built-in screen is an output like the
  others.
- Writes from QML go out at once; coalescing is the helper's job (§5.4), so
  the model stays simple and every client behaves the same.
- **What the slider shows** follows the rule of the service-backed switches
  (`shell/src/requested.h`): the value asked for at once, then the
  compositor's once it has taken that request; an answer to an older
  request leaves the newest alone. The compositor numbers a target only
  when it changes what is shown, so the model knows the `seq` its request
  will get (one more for each value it sends that rounds differently) and
  ignores every state with a smaller one: a drag never pulls the knob back.
  From then on `pending` and `refused` are the compositor's (§7.1). A
  request never taken (the output stopped being ready meanwhile) is dropped
  after 3 s.
- **Quick settings** shows the slider of the output it opens on
  (`QScreen::name()`); the page lists every output with a slider, by count,
  so a new state never recreates a slider being dragged.
- The model reconnects every second when the compositor restarts, and
  sends `watch brightness` and `brightness-live` again.

## 10. Later

- **HDR (renderer S7).** In HDR many external monitors lock their
  brightness (§4.2): DDC/CI writes are `refused`. S7 decides what the slider
  means there; GNOME's answer is to dim in the signal, which 10- and 12-bit
  HDR allow. That would be another `mechanism` of the same output model, not
  the SDR software dimming excluded in §2.
- **KMS backlight.** When the kernel's KMS backlight API lands, the built-in
  backlight can move into the compositor and change atomically with a frame
  (§4.2). The model and the clients don't change.
- **Brightness keys and OSD**: the monitor under the pointer, on the same
  model and commands.
- **Power policy** (idle dimming, battery, ambient light): too early. When it
  comes, it is one more client that asks the compositor for temporary
  changes, and the user's value stays separate from them.

## 11. Not in scope

- **Software dimming in SDR**, for monitors without DDC/CI or below the
  monitor's minimum (§2).
- **All monitors on one slider** (§2).
- **Contrast, input source, color presets** and other VCP features.
- **USB monitors** (DDC/CI over USB HID).

`vela-brightness --probe [connector]` is in scope: each connector, its bus,
the transfer mode, and one read with every attempt, its bytes and its
latency. It only reads, so it is safe anywhere, and its output goes into the
bug report in B4. `vela-brightness --bench <connector>` measures the timings
of §5.4 on a real monitor: it writes, and puts the starting value back.

## 12. Tests

- **Protocol, pure:** encoding and checksums of both messages; parsing of the
  replies captured in §3, the good ones, the Mi's fixed block, null messages,
  short reads.
- **Scheduler, pure:** §5.4 against a fake clock and a fake bus: coalescing
  during a drag, the 250 ms interval, the read back and the rewrite after a
  dropped write, the budget and `refused`, a stale read back with an old
  `seq`, `not-responding` and its recovery, live sync skipping a pending or
  recent write, `flock` busy, a suspend (the boot clock jumping ahead of the
  monotonic one) in the middle of a drag and of live sync.
- **Fake monitors:** a fake backend, chosen with
  `VELA_BRIGHTNESS_FAKE=<file>`, describes monitors and their faults, from §3,
  with a **fixed seed** so every run is the same: ignore a write that comes
  5–50 ms after another (the LG), drop writes and return the fixed block (the
  Mi), no answer for a while after `power on`, a value changed "by the
  buttons", a value locked (refused), no DDC/CI at all, a transfer that
  hangs. It binds to outputs by name, so headless outputs (`HEADLESS-1`) get
  a monitor.
- **Fake backlight:** `VELA_BRIGHTNESS_SYSFS=<tree>` (with fake monitors
  only) is a sysfs tree of the test's own: the compositor chooses backlights
  in it and the helper writes its files, never logind. The choice of §5.1 and
  the backlight backend are unit-tested on trees shaped like i915's, amdgpu's
  and ACPI's.
- **The shell's model** (`shell/tests/brightness_model.cpp`): against a fake
  compositor socket, an older state, a drag, a refusal, a request never
  taken, a locked output, the wheel's steps, a compositor restart.
- **Settings' card** (`settings/tests/tst_brightnesscard.qml`, Qt Quick
  Test): the slider and a click on it, locked, refused, the text of each
  reason, and no card where there is nothing to say.
- **Regressions from COSMIC (§4.3):**
  - #165: no device is open while a helper is idle, during the pauses of a
    drag, or after its output is removed;
  - #183: no read overlaps a write on the same device;
  - #1483: the number of transactions in a drag is bounded and known;
  - #150: dragging one output's slider never touches another output.
- **Functional** (`tests/functional/test_brightness.py`, its own ctest
  entry `functional-brightness`): the headless harness with the fake
  backend: `brightness`, `watch brightness` (including a watcher that stops
  reading), live sync and its count per connection, hotplug with
  `test-output`, a disabled output, `test-power off/on`, the watchdog killing
  one hung helper while another output keeps working, a helper that keeps
  crashing, the fake backlight; Settings' Display page probing a monitor
  that woke up after the helper's own probes; in the real shell, the quick settings slider
  (a click and a drag move the display, found in screenshots), the page with
  a slider per display, and the slider following the monitor's buttons only
  while the panel is open. Headless outputs never get the same name twice,
  so two identical monitors and a fast replug of the same connector are left
  to the manual tests.
- **Manual, on real hardware** (added to [hardware](hardware.md)): every
  monitor gets a slider or the right reason, a drag ends on the value shown,
  0% and back, the monitor's buttons move an open slider, unplug and replug,
  idle off and back on, VT switch, suspend and resume with Settings open, `ddcutil getvcp 10` agreeing with the
  slider; NVIDIA (the `read()`/`write()` fallback) and Intel; a laptop
  (backlight choice, minimum, OLED if available); a dock or MST hub
  (#165). `ddcutil` is the reference, not a dependency.

## 13. Packaging and documentation

- Vela installs its own files (`brightness/`); `ddcutil` is not needed:
  - `/usr/lib/udev/rules.d/70-vela-i2c.rules`:
    `SUBSYSTEM=="i2c-dev", KERNEL=="i2c-[0-9]*", ATTRS{class}=="0x03*", TAG+="uaccess"`
    (the I²C buses of display adapters, for the session's user; the same
    rule as ddcutil's, harmless if both are installed);
  - `/usr/lib/modules-load.d/vela-i2c.conf`: `i2c-dev`.
- PKGBUILD: `systemd-libs` for `sd-bus` in the helper (§5.3); without
  libsystemd at build time the helper can't change a laptop's backlight.
- Vela Report: an optional "Display brightness" collection with `--probe`
  and the compositor's `brightness` answer ([reporting](reporting.md#5-collections-and-explanations)).
- Fedora (`packaging/fedora/vela.spec`): `BuildRequires: systemd-devel`, and
  the udev rule and the `modules-load.d` file in `%files`.
- [hardware](hardware.md):
  - many monitors ship with **DDC/CI turned off** in their own menu;
  - with NVIDIA's proprietary driver, I²C may need the driver options that
    ddcutil documents (`nvidia-i2c.conf`);
  - the manual tests of §12.
- [configuration](configuration.md): `brightness`, `VELA_BRIGHTNESS`,
  `VELA_BRIGHTNESS_FAKE`.

## 14. Open questions and risks

- **Our own DDC/CI meets the world.** ddcutil has years of quirks behind it:
  monitors that need longer waits, ones that answer only after a null
  message, drivers with broken I²C. The read back, the retries and the
  transfer fallback cover a lot of it; what they don't cover shows up in the
  manual tests and in `--probe` reports. ddcutil's sources and issues are
  the reference when a monitor misbehaves.
- **EEPROM wear.** Many monitors save each change. Four writes a second
  while dragging is more than PowerDevil's two, for a few seconds at a time;
  if that proves a concern, the interval in §5.4 grows and the read
  back still guarantees the final value.
- **Live sync traffic.** One read a second per output, only while a slider is
  on screen. To check that it never disturbs the picture or the monitor's own
  menu.
- **Choosing the built-in backlight** (§5.1) is a heuristic; laptops with
  two backlights for one panel exist (§4.2).
- **D state.** §6 limits the damage of a transfer stuck in the kernel; it
  can't prevent it.
- **Monitors behind docks and MST hubs.** DDC/CI often doesn't pass through
  them; they show `no-answer`, which is correct.

## 15. Stages

| Stage | Content | |
|---|---|---|
| B0 | `vela-brightness`: DDC/CI protocol and transfers, scheduler, fake backend and their unit tests, `--probe`; numbers tuned on the monitors of §3 | done |
| B1 | Compositor: a helper per output with its watchdog, the lines of §5.6, the model, `brightness`, `watch brightness`, `brightness-live`, `brightness-probe`; functional tests | done |
| B2 | Backlight backend (§5.3); shell: `Brightness` model, quick settings slider for the panel's output and the Brightness page, live sync; backlight out of `SystemStatus` | done |
| B3 | Settings: the Display page card for the selected output, with the reasons of §8 | done |
| B4 | udev rule, `modules-load.d`, PKGBUILD, docs, `--probe` in the bug report (§13); the user's tests on real hardware | done but the tests on real hardware |
