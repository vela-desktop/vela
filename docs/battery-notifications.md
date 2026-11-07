# Battery notifications

Design of the notifications Vela shows when a battery runs low: the PC's
own and the peripherals' (mice, keyboards, headsets, game controllers),
whatever way they connect (a Logitech receiver, Bluetooth, USB). Only
UPower is read; there is no backend for any one vendor.

> **Status:** implemented (§9); a check in a real session, with a real
> mouse running low and a laptop, remains.

Battery notifications were left out of the first Bluetooth work
([bluetooth.md §3](bluetooth.md#3-scope)) to keep it smaller; this
is where they come back, for every device UPower knows.

## 1. Goals

1. **Right, or silent.** A notification only when the device's own report
   says its battery is low. A guess (a percentage UPower itself marks
   "should be ignored", a 0% before the first reading) never warns.
2. **Once.** Low is told once, and critical once more. A reading that
   bounces between low and normal, a device that reconnects, UPower
   restarting: none of them says it again.
3. **No polling.** Signals only: devices added and removed, each device's
   PropertiesChanged, UPower's owner.
4. **The user decides.** The PC's notifications can be turned off and their
   low level chosen; the peripherals' can be turned off.

## 2. Decisions taken

| Decision | Choice | Consequence |
|---|---|---|
| Source | **UPower only** (§3) | HID++ receivers, Bluetooth (BlueZ's Battery1) and USB devices alike; nothing vendor-specific |
| When a peripheral is low | **UPower's `WarningLevel`** (§4.1) | its thresholds and `UPower.conf` decide, already weighing the coarse `BatteryLevel`; Vela has no thresholds of its own for them |
| When the PC is low | **the user's level**, 20% by default (§4.2) | Settings > System > Power; critical stays UPower's, just before its critical action |
| Rearming | **charging**, or **coming back** with a normal charge (§4.4) | a normal reading while connected doesn't rearm: voltage-based readings bounce |
| Do not disturb | low ones respect it, **critical ones pass** (§5) | the low one waits in the notification center |
| How long critical stays | **the PC's until dismissed**, a peripheral's **expires** like others (§5) | `x-vela-expires`, a hint of Vela's own |
| Switches | the PC's battery (both levels) in Power, the peripherals' in Bluetooth & devices (§6) | off: nothing at all, critical included |

## 3. Where the readings come from

`UPowerBatteries` (shell/src/upowerbatteries.h) follows UPower on the
system bus:

- `EnumerateDevices` at start and whenever UPower gets a new owner, and the
  **DisplayDevice**, which isn't enumerated: the PC's batteries together.
- `DeviceAdded` and `DeviceRemoved`.
- Each device's `PropertiesChanged`, which refreshes **that device only**:
  the signal's own path (QDBusContext), not every device.
- Each reading is a `GetAll`, async. A reply older than the last request
  for the device, or from a previous UPower, is dropped. Requests are
  numbered across devices, so a device removed and added again at the same
  path never takes a reply owed to its previous self.
- A read that fails (UPower busy, a timeout) is asked again after 1, 2 and
  3 seconds, then left until the device's next signal.

Never followed after their first reading: line power, UPSs and the power
supply batteries (BAT0, BAT1), which are in the DisplayDevice already. A
laptop's BAT0 changes often, and would otherwise cost a read each time.

## 4. When to tell

`BatteryAlerts` (shell/src/batteryalerts.h) is the whole policy, pure:
readings go in, "show" and "close" come out. It is tested without D-Bus.

### 4.1 Peripherals: UPower's warning level

UPower already computes `WarningLevel` for every device: from the coarse
`BatteryLevel` when the device reports one (HID++ mice: "low", "normal"),
otherwise from the percentage and its own thresholds, which the system can
change in `UPower.conf`. Vela takes it as it is: Low is low, Critical and
Action are critical, None is normal.

Two exceptions, both about not trusting a guess:

- `BatteryLevel` **Unknown** (not None): nothing is known, whatever
  `WarningLevel` or the percentage say.
- **0%** from a device with no coarse level that isn't discharging: a
  device just connected, before its first real reading.

A device is not present when it is of unknown type, or a battery bay
(`Type` Battery) with `IsPresent` false. Bluetooth headsets report
`IsPresent` false while giving their charge, so it counts only for bays.

### 4.2 The PC: the user's level

The DisplayDevice is present when it is a battery and `IsPresent`. While it
discharges, low is **at or under the user's level** (`battery/lowLevel`,
§6), not UPower's low warning: the user's choice wins. Critical is UPower's
`WarningLevel` Critical or Action, just before UPower's critical action
(hibernate, power off). With a state other than charging or discharging
(a charge limit reached, unknown), nothing is told, critical included.

### 4.3 Who a device is

A device's identity is its **vendor and serial**, otherwise its **native
path**, otherwise its object path. HID++ devices can come back under
another object path (`hidpp_battery_0`, then `_1`); the serial stays. The
identity is fixed at a device's first reading for as long as it stays: a
serial that shows up later doesn't make it another device.

### 4.4 Once, and rearming

For each device, what was told: nothing, low or critical.

- **Higher than told:** told. Critical after low closes the low one and
  pops up on its own, even if the low one already waits in the
  notification center.
- **Lower or equal:** nothing.
- **Charging**, fully charged or waiting to charge: **rearmed**, and the
  notification closes, from the notification center too.
- **Normal while connected:** nothing. HID++ batteries measured by voltage
  bounce between low and normal; rearming there would warn again and again.
- **Removed:** what was told stays. Back with the same level: nothing.
  Back with a normal charge (the batteries were replaced) at its first
  known reading: rearmed.
- **Unknown:** changes nothing.
- **The PC's level moved while its charge isn't known:** the first known
  reading decides, as if it had moved then (§6).
- **Dismissed** by the user: still told, until it rearms.

The state lives only in the shell: after the shell restarts, a battery
still low is told once more.

## 5. The notifications

`BatteryNotices` (shell/src/batterynotices.h) shows them through Vela's
notification server.

| | App | Title | Text | Urgency | Expires |
|---|---|---|---|---|---|
| PC, low | Power | Battery low | "18% remaining. Plug in your PC." | normal | like others |
| PC, critical | Power | Battery critically low | "4% remaining. Plug in your PC now." | critical | **no**, until dismissed or plugged in |
| Peripheral, low | Devices | "Logitech MX Vertical battery is low" | "Recharge the device soon.", or "15% remaining. Recharge the device soon." | normal | like others |
| Peripheral, critical | Devices | "... battery is critical" | "Recharge the device now.", or "3% remaining. ..." | critical | **yes**, into the notification center |

The percentage is there only when it is real. A device with a coarse
`BatteryLevel` (HID++ mice: the kernel's `capacity_level`, no `capacity`)
has none: UPower makes one up from the level (low is 10%, "should be
ignored") and Vela leaves it out rather than show a number the device
never gave. Headsets and keyboards over Bluetooth report a real one.

A peripheral without a model is named by its kind ("Mouse battery is low",
"Headset", "Game controller"...), "Wireless device" when the kind isn't
known; its icon is its kind's (`input-mouse`, `audio-headset`...).

Critical notifications pass Do not disturb, the user's and the one while
the screen is shared ([screen sharing](screen-sharing.md) §7); low ones
wait in the notification center. A critical notification normally stays
until dismissed, even when more than four popups crowd it (the oldest
others go into the notification center first); `x-vela-expires` (a boolean hint, Vela's own) makes it
expire like any other: a mouse at its last charge needs to be seen, not to
stay on screen.

## 6. Settings

In `vela-shell.conf` ([configuration](configuration.md#shell-settings)),
written by Settings and followed live by the shell:

| Key | Default | Settings |
|---|---|---|
| `battery/notify` | `true` | System > Power > Battery notifications: the PC's, low and critical |
| `battery/lowLevel` | `20` | System > Power > Low battery level: 10, 15, 20, 25, 30, 40 or 50%; the file may hold any whole number from 5 to 95, anything else is 20 |
| `battery/devices` | `true` | Bluetooth & devices > Low battery notifications: the peripherals' |

The Power group shows only when the PC has a battery. The peripherals'
switch shows even without Bluetooth: receivers are devices too.

Changes apply at once. Turned off: whatever is shown closes. Turned on: a
battery already low is told. The level raised over the charge: told now;
lowered under it: the low notification closes and comes again at the new
level. A critical one is never closed by the level.

## 7. Not in scope

- Battery levels shown for peripherals outside the Bluetooth page (a
  devices panel in quick settings, the tray). Settings' Bluetooth page
  already shows BlueZ's levels.
- UPower's critical action itself: it's the system's (`UPower.conf`).
- Remembering what was told across shell restarts (§4.4).
- `SystemStatus` (the taskbar's battery) keeps its own reading of the
  DisplayDevice.

## 8. Code

| File | Role |
|---|---|
| shell/src/upowerbatteries.h | UPower on D-Bus, readings out (§3) |
| shell/src/batteryalerts.h | the policy, pure (§4) |
| shell/src/batterynotices.h | readings in, notifications out (§5) |
| shell/src/batterysettings.h | the `[battery]` keys, read alike by the shell and Settings (§6) |

## 9. Tests

| Test | What it covers |
|---|---|
| `shell-battery-alerts` (unit) | reading UPower's properties (warning level, unknown coarse level, 0% before the first reading, presence, states, identity, what isn't followed); the policy: once, escalation, bouncing, charging, reconnecting, devices apart, the PC's level, the switches and the level moving |
| `shell-battery-notices` | a fake UPower on a private bus, end to end: the notifications at start, only the sender read, BAT0 not followed, bouncing and reconnecting under another path, UPower restarting, a late reply dropped (also for a device added again at the same path), failed reads retried, the PC's battery and plugging in, settings live, Do not disturb and expiry, dismissed, an identity that stays, no UPower |
| `shell-notification-silence` | `x-vela-expires`; a critical notification that stays kept on screen when the popups are full |
| `shell-config`, `settings-preferences` | the `[battery]` keys, read and written, a bad level ignored |

Each of these was checked by mutation: every rule above removed or
inverted in turn (only the sender read, BAT0 unfollowed, the rearming
rules, `x-vela-expires` sent and honored, the user's level, UPower's
critical, the identity kept, the low one closed on critical, the two
guesses, the switches and the level, stale replies and requests numbered
across devices, failed reads retried and their bound, the DisplayDevice
added, the level's range, no critical for the PC outside discharging, the
level moved while unknown, a critical one that stays kept on screen) makes
at least one of them fail.
