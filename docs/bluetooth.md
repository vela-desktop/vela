# Bluetooth

Design of Vela's Bluetooth, rewritten from scratch. It covers devices,
pairing, quick settings, nearby devices ready to pair, and diagnostics.
This document comes before the code: decisions are made here, and the code
follows them.

> **Status:** design under discussion in PR #146. No code yet. Everything in
> §3 is in scope for beta 0.1. Stages are listed in §13.

## 1. Why from scratch

Bluetooth today is four pieces that don't talk to each other:

- **Settings** (`settings/src/bluetooth.cpp`) has its own model.
  - It pairs without an `org.bluez.Agent1`. A device that needs a
    confirmation, a PIN or a passkey cannot pair unless another desktop's
    agent answers. A mouse paired in Plasma but not in Vela.
  - It ignores the replies of `StartDiscovery`, `StopDiscovery`,
    `RemoveDevice` and `Set Trusted`.
  - It reads BlueZ synchronously, in the UI thread.
  - It does not notice a `bluetoothd` restart.
  - A device's `busy` flag has no guaranteed way out.
- **The shell** (`shell/src/systemstatus.cpp`) reads the adapter for the
  quick settings switch, again synchronously.
- **`vela-audio`** reads Bluetooth batteries from BlueZ by itself.
- **Files** shares through `bluedevil-sendfile`, a KDE program.

## 2. Goals

1. **Bluetooth works with only `bluez` installed.** No KDE or GNOME
   component is needed. BlueDevil is removed.
2. **It never says yes on the user's behalf.** A device the user did not ask
   for cannot pair or use a service without a visible "Allow".
3. **Operations outlive windows.** A pairing started in Settings finishes even
   if Settings is closed, or the shell restarts.
4. **Nothing gets stuck.** Every operation ends in "done", "failed" (in plain
   words) or "cancelled". This holds when the device disappears, the adapter
   is unplugged or `bluetoothd` restarts.
5. **"Bluetooth doesn't work" always has a reason.** Where Vela can fix it,
   a button fixes it (§9).
6. **It costs nothing while idle.** Vela runs no active scan unless the user
   is looking for a device. Nearby devices are noticed by a passive monitor
   (§6.6).
7. **It can be tested without a radio.** Every automated test runs against a
   fake BlueZ on a private bus.

## 3. Scope

In beta 0.1:

- Pairing with Vela's own agent, for every pairing method (§6.4).
- Connecting, disconnecting, removing and renaming devices. Each device
  shows its type and its battery level, if it reports one.
- The PC is discoverable only while "Add a device" is open.
- A notification when a nearby device is ready to pair, like Windows' Swift
  Pair (§6.6). A default BlueZ doesn't allow it yet: decision pending
  (§14).
- Quick settings: a Bluetooth device list with connect and disconnect
  (§10.2).
- Codec and profile of headsets: they already exist in `vela-audio`
  ([audio](audio.md) §6.2). The Bluetooth page links to them (§10.1).
- Diagnostics with fixes (§9).
- Robustness: `bluetoothd` restarts, adapters plugged in and out, several
  adapters, rfkill.

Decided out:

- **Battery notifications.** Battery levels are shown, but low battery is
  never notified.
- **File transfer (OBEX)**, in either direction. "Send via Bluetooth" goes
  away with BlueDevil (§11).

Not discussed yet, so not in 0.1: tethering (PAN), Google Fast Pair, and
any UI specific to LE Audio. Whatever PipeWire supports still plays.

## 4. Decisions taken

| Decision | Choice | Consequence |
|---|---|---|
| Process | **`vela-bluetooth`**, a session daemon started and supervised by the compositor, like `vela-polkit-agent` | pairings survive Settings and the shell; it is in the compositor's logind session (§6.1) |
| Language and libraries | **C++ with QtCore and QtDBus**, no GUI | no KF6 and no BluezQt: Vela doesn't use KDE Frameworks |
| Who talks to BlueZ | **only `vela-bluetooth`** | one model, one agent, one place for errors |
| Client API | **`org.vela.Bluetooth`** on the session bus (§7) | Settings, the shell and `vela-audio` are clients |
| Client library | **`common/bluetooth`**: a C++ model and a QML module | no client parses D-Bus by hand (§8) |
| Agent | in the daemon, with capability **`KeyboardDisplay`**, default agent **while the session is active** | every pairing method works. BlueZ asks the agent of `Pair`'s own caller first (`agent_get(sender)` in `src/device.c`), so Vela's pairings use Vela's agent even when another desktop holds the default. Unsolicited requests reach Vela only while it is in front |
| Where questions appear | **inline in Settings** if Settings started the operation and is open; otherwise a **shell dialog**; requests the user did not start appear first as a **notification** | the user answers where they are looking (§10.3) |
| Discoverable | **only while "Add a device" is open**, as a lease held by Settings (§6.5) | the PC is not visible to everyone all day |
| Nearby devices | **BlueZ's `AdvertisementMonitor1`**, watching for Swift Pair beacons, only with patterns offloaded to the controller (§6.6) | no active scan; **not available on a default BlueZ**: how to enable it is still to decide (§14) |
| Codec and profile | **stay in `vela-audio`**: PipeWire owns them | the daemon never touches audio |
| Battery in `vela-audio` | read **from `vela-bluetooth`** instead of BlueZ | one Bluetooth client |
| BlueDevil | **removed** | §11 |

## 5. Overview

```
bluetoothd (system bus)          systemd, logind, /dev/rfkill
      ▲  Adapter1, Device1, AgentManager1, AdvertisementMonitorManager1
      │  ◄── Agent1, AdvertisementMonitor1 (served by vela-bluetooth)
      ▼
vela-bluetooth (child of vela-compositor, no GUI)
      │  org.vela.Bluetooth (session bus)
      ▼
common/bluetooth (client library, QML module Vela.Bluetooth)
      ├── vela-shell: quick settings, question dialog, notifications
      ├── vela-settings: Bluetooth page
      └── vela-audio: headset batteries
```

## 6. `vela-bluetooth`

### 6.1 Start and lifetime

- `vela-compositor` starts it and restarts it if it crashes. This works like
  the polkit agent ([polkit agent](polkit-agent.md) §4.1, and
  `compositor/src/polkit.h`).
- By default it starts only in the real (DRM) session. Nested and headless
  sessions don't start it, because they must not register an agent for the
  user's real Bluetooth. `VELA_BLUETOOTH` names another command, or `0` for
  none. The tests use it with a private system bus.
- The daemon owns `org.vela.Bluetooth`. A second instance exits.
- Without `org.bluez` on the bus, it stays running and reports why (§9).
  When BlueZ appears, it starts working.

### 6.2 The model

- At start, and whenever `org.bluez` reappears, the daemon reads
  `GetManagedObjects` asynchronously. From then on it follows
  `InterfacesAdded`, `InterfacesRemoved` and `PropertiesChanged`.
- When `org.bluez` leaves the bus, adapters and devices are cleared at once.
  Operations fail and questions close (§6.3, §6.4).
- **The adapter in use:**
  - It is the first adapter in BlueZ's object tree, unless the user chose
    another one.
  - If it disappears, the next adapter is used.
  - The choice is remembered by address, so a USB dongle that comes back is
    picked again.
- **Devices** are those of the adapter in use. Each one has:
  - path, address, name (the `Alias`) and type (from `Icon` and `Class`);
  - paired, trusted, connected and blocked;
  - battery level (`Battery1`), or none;
  - signal strength (only while searching);
  - its operation and its open question, if any.
- **The adapter** has:
  - name;
  - powered, and its `PowerState`;
  - discoverable and searching.

### 6.3 Operations

At most one operation runs per device. A second request for the same thing
joins the running one. A different request is refused with "busy".

| Operation | Steps | Notes |
|---|---|---|
| **Add** | `Pair` → `Set Trusted` → `Connect` | `AlreadyExists` counts as paired. If `Trusted` fails, the failure is shown but the operation goes on. If `Connect` fails, the device stays paired with "Paired, but couldn't connect" |
| **Connect** | `Connect` | |
| **Disconnect** | `Disconnect` | |
| **Remove** | `Adapter1.RemoveDevice` | ends when `Device1` disappears |
| **Rename** | `Set Alias` | |

- Every step has a D-Bus timeout longer than BlueZ's own, so BlueZ's error
  normally arrives first. BlueZ gives an agent 60 seconds per question
  (`REQUEST_TIMEOUT` in `src/agent.c`). `Pair` gets 90 seconds; the other
  steps get 30.
- **An operation ends early** when its device's `Device1` disappears, the
  adapter stops being powered, or `org.bluez` leaves the bus. Each case has
  its own message.
- **Cancel:**
  - while pairing, the daemon calls `Device1.CancelPairing` and closes the
    open question;
  - while connecting, it calls `Disconnect`.
- **Errors:** BlueZ's error name and message go to the daemon's log. The
  client gets a short, stable code and a plain-words message, for example:
  - "The device isn't responding. Make sure it's on and nearby.";
  - "Pairing was refused on the device.";
  - "Bluetooth is busy. Try again in a moment.";
  - "Bluetooth isn't running."

  The user's own Cancel gives no message.

### 6.4 The agent

The agent is served at `/org/vela/BluetoothAgent` on the system bus. It is
registered with `AgentManager1.RegisterAgent(…, "KeyboardDisplay")` at
start, and again whenever `org.bluez` reappears. `RequestDefaultAgent` is
sent while the logind session is active, and again each time it becomes
active.

| BlueZ call | What the user sees | Answer |
|---|---|---|
| `RequestConfirmation(device, passkey)` | "Does *Name* show 123456?" Pair / Cancel | empty reply, or `org.bluez.Error.Rejected` |
| `RequestAuthorization(device)` | "*Name* wants to pair" Allow / Deny | same |
| `AuthorizeService(device, uuid)` | "Allow *Name* to use *service*?" Allow / Deny | same |
| `RequestPasskey(device)` | a 6-digit field | the number |
| `RequestPinCode(device)` | a text field (1–16 characters), unless the legacy rule applies (below) | the string |
| `DisplayPasskey(device, passkey, entered)` | "Type 123456 on *Name*, then Enter", with the digits typed so far | none: it closes when the pairing ends |
| `DisplayPinCode(device, pincode)` | "Type 0000 on *Name*, then Enter" | empty reply at once |
| `Cancel()` | the question disappears; the operation fails with "The device isn't responding" | — |
| `Release()` | — | the daemon registers again when BlueZ comes back |

- **Legacy PIN** (`RequestPinCode`), only for an Add the user started:
  - a keyboard (`Icon` `input-keyboard`) gets a random 6-digit PIN, shown
    as with `DisplayPinCode`;
  - mice, gamepads and audio devices get `0000`;
  - every other device, and every request the user did not start: the user
    types the PIN.
- Nothing else is answered without the user. BlueZ authorizes services of
  trusted devices by itself, without the agent (`src/adapter.c`). Devices
  Vela pairs are trusted, so `AuthorizeService` only comes from devices
  paired elsewhere, or from devices the user marked as not trusted.
- **Unsolicited requests while the session is not active** are rejected.
  Another session's agent should have them.
- **Order:** one open question per device. Questions for different devices
  wait in a queue.

### 6.5 Searching and being discoverable

- Clients don't call `StartDiscovery` themselves. They take a **search
  lease** with `StartSearch()`, which returns an id.
- The lease ends with `StopSearch(id)`, or when its owner leaves the bus.
- While at least one lease is held:
  - BlueZ discovers;
  - the adapter is `Discoverable`, with a `DiscoverableTimeout` of
    180 seconds that the daemon renews.

  When the last lease ends, both stop. If the daemon crashes, BlueZ's
  timeout still turns discoverability off.
- **Add** suspends discovery while it pairs, because discovery disturbs
  pairing. It resumes it afterwards if leases are still held.

### 6.6 Nearby devices ready to pair

- While Bluetooth is on and the setting is enabled (on by default), the
  daemon registers an `AdvertisementMonitor1` with
  `AdvertisementMonitorManager1`. The monitor is of type `or_patterns` and
  matches Microsoft's Swift Pair beacon. That is manufacturer data (AD type
  `0xFF`) made of:
  - `06 00`: Microsoft's company ID, `0x0006`;
  - `03`: the Swift Pair beacon ID;
  - one byte for how the device pairs: `00` LE only, `01` BR/EDR only,
    `02` both;
  - `80`, a reserved byte.

  The pattern is `06 00 03`, so all three kinds match. Sources: Microsoft's
  [Swift Pair guide](https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/bluetooth-swift-pair)
  and Nordic's Swift Pair payload (`swift_pair.c` in `sdk-nrf`).
- **Only with offload.** The monitor is used only when
  `AdvertisementMonitorManager1.SupportedFeatures` contains
  `controller-patterns`, meaning the controller filters advertisements
  itself. Windows does the same: without hardware offload the feature is
  not offered. Continuous listening on the host costs power and disturbs
  Wi-Fi on combined radios.
- **A default BlueZ doesn't offer it.** `AdvertisementMonitorManager1` and
  `AdvertisementMonitor1` are experimental. `bluetoothd` creates the
  manager only with `-E` or `Experimental = true` (`src/adapter.c`, 5.86
  and 5.87). Arch (5.87), Fedora 44 (5.86) and Ubuntu 26.10 (5.87) all
  ship without it. How Vela gets it is an open decision (§14).
- `DeviceFound(device)` posts a notification: "*Name* is ready to pair".
  Its actions are "Pair", which starts an Add, and "Dismiss".
  - The same device is notified at most once every 5 minutes.
  - Paired devices are never notified.
  - Nothing is notified while "Add a device" is open: it already lists the
    device.
- If BlueZ doesn't offer `AdvertisementMonitorManager1` (some versions need
  `Experimental`), or the controller can't filter by itself, the feature is
  unavailable. Settings says so. Vela never falls back to an active scan.

## 7. `org.vela.Bluetooth`

Served at `/org/vela/Bluetooth` on the session bus. This sketch is not
final; the exact signatures are settled in B1.

```
properties
  Status          s          ok, or the problem's code (§9)
  Adapters        aa{sv}     address, name, path, in use
  Adapter         a{sv}      the adapter in use (§6.2)
  NearbyPairing   b          setting of §6.6 (read/write)
  NearbyAvailable b
methods
  Devices() → a{oa{sv}}
  SetPowered(b on)
  UseAdapter(s address)
  StartSearch() → u          lease (§6.5)
  StopSearch(u lease)
  Add(o device, b promptsHere)
  Connect(o device) / Disconnect(o device) / Remove(o device)
  Rename(o device, s name)
  Cancel(o device)
  Answer(u question, b accept, s value)
  Fix()                       the fix for the current Status (§9)
signals
  DevicesChanged(a{oa{sv}} changed, ao removed)
  OperationChanged(o device, s state, s error, s message)
  Question(u id, o device, s kind, s code, b unsolicited)
  QuestionClosed(u id)
```

`promptsHere = true` means that the caller draws the questions for this
operation (§10.3). If the caller leaves the bus while a question is open,
the question moves to the shell.

## 8. `common/bluetooth`

A client library, like `common/audio` for sound:

- a C++ model of adapters and devices, fed by `DevicesChanged`;
- a QML module, `Vela.Bluetooth`, with `devices`, `paired`, `found`,
  `status` and the actions;
- the `requested.h` behaviour for switches: shown at once, and returned to
  BlueZ's state if BlueZ refuses or never does it.

Users: `vela-shell` (quick settings, the question dialog and notifications,
`SystemStatus` for airplane mode), `vela-settings`, and `vela-audio` for
headset batteries. If the daemon is missing, the model is empty, with status
`daemon-missing`.

## 9. Diagnostics

`Status` names the first problem found, in this order:

| Status | When | What the user sees | Fix |
|---|---|---|---|
| `not-installed` | `org.bluez` is neither on the bus nor activatable | "Bluetooth isn't installed (package `bluez`)." | — |
| `service-off` | `bluetooth.service` is inactive or disabled (systemd over D-Bus) | "The Bluetooth service is turned off." | **Turn on**: `StartUnit` and `EnableUnitFiles`, authorized by the polkit agent |
| `no-adapter` | BlueZ runs but has no adapter | "No Bluetooth adapter found." | — |
| `hard-blocked` | `/dev/rfkill` reports a hard block | "Bluetooth is turned off by a switch or key on the PC." | — |
| `soft-blocked` | soft block (Plasma leaves it, for example) | the switch shows Off | the switch unblocks it (as today, `bluetoothpower.h`) |
| `adapter-error` | `PowerState` stays `off-enabling`, or `Powered` is refused, after the retries | "The Bluetooth adapter didn't turn on." | **Try again** |
| `daemon-missing` | client side: `org.vela.Bluetooth` absent | "Bluetooth isn't available in this session." | — |

The daemon watches `/dev/rfkill` for events. `VELA_RFKILL` replaces it in
the tests, as it does today.

## 10. UI

### 10.1 Settings

The Bluetooth page is rewritten on `Vela.Bluetooth`. The current
`settings/src/bluetooth.*` is deleted.

- At the top: the switch. A problem from §9 replaces it with its text and
  fix button.
- **Paired devices**, connected first. Each one shows:
  - its icon, name and state ("Connected", "Paired", "Connecting...",
    "Pairing...", "Waiting for you...", or the last error);
  - its battery level;
  - a menu with Connect or Disconnect, Rename and Remove;
  - for headsets, "Sound settings", which opens the audio device page with
    the codec and profile.
- **Add a device** opens a dialog. It takes a search lease, which also makes
  the PC discoverable (shown as "Discoverable as *name*"). It lists the
  devices found, nearest first. Clicking one starts an Add. Its questions
  replace the list in the same dialog. On success the dialog closes.
- **Advanced:**
  - the adapter, if there are two or more;
  - "Show notifications to pair nearby devices" (§6.6), hidden or disabled
    with the reason when unavailable.

### 10.2 Quick settings

- The Bluetooth tile gets an arrow, as Wi-Fi has. It opens a list of paired
  devices with their battery level and connect or disconnect, plus "More
  Bluetooth settings".
- Unpaired devices are not listed there.

### 10.3 Questions from the agent

- **Settings started the operation and is open:** the question appears
  inline (§10.1).
- **Otherwise:** a shell dialog. It is a small window centered on the
  primary screen, styled like `ConfirmDialog`, not the full-screen polkit
  prompt.
  - It shows the device's icon and name, the code in large digits, and the
    field when one is needed.
  - Enter confirms and Esc cancels. The field has focus.
  - Every element has an accessible name. The code is read digit by digit.
  - The dialog closes by itself on `QuestionClosed`.
- **Unsolicited** (a phone or headset starting the pairing, or a service
  request): a notification first.
  - It shows the code when there is one.
  - Its actions are "Pair" or "Allow", and "Deny". For a PIN or passkey to
    type, the action is "Enter code", which opens the dialog.
  - If the notification closes without an answer, the request is rejected.

## 11. Removing BlueDevil

- Files: the "Bluetooth" entry in Share (`shell/qml/FileDialogs.qml`), and
  `FileActions::canBluetooth` and `sendByBluetooth`
  (`shell/src/fileactions.*`).
- Packaging: the `bluedevil` optional dependency in `packaging/arch/PKGBUILD`.
- Docs: every mention.

`bluez` stays as today: `Recommends` on Fedora and an optional dependency on
Arch. [installation](installation.md) says it is all Bluetooth needs.

## 12. Tests

**Fake BlueZ.** This is a shared helper in `tests/harness/`, grown out of
`FakeServices` in `shell/tests/requested_switches.cpp`. It runs on a private
system bus, in its own thread. It serves:

- `ObjectManager`, `AgentManager1`, `Adapter1`, `Device1`, `Battery1` and
  `AdvertisementMonitorManager1`;
- a fake `systemd1` (unit state, `StartUnit`) and a fake logind (`Active`).

Each fake device follows a script:

- its pairing method: Just Works, confirmation, passkey entry, passkey
  display, or legacy PIN with the PIN it accepts;
- whether `Connect` succeeds;
- its delays.

On `Pair`, the fake calls the registered agent as BlueZ does. It can also:

- leave the bus and come back;
- remove a device or the adapter in the middle of an operation;
- send `Cancel` and `Release`;
- refuse any call;
- report an advertisement.

**Daemon tests** (`bluetooth-*`):

- the model, and rebuilding it after a restart;
- the adapter fallback, and remembering the chosen adapter;
- each row of §6.3, and each early ending;
- each row of §6.4, and the legacy PIN rule;
- the default agent following `Active`;
- search leases ending when their owner leaves;
- the monitor and its notification limits;
- each status of §9, and the fixes;
- no operation or question left open after any of the cases above.

**Client library tests:**

- the model against the daemon running on the fake;
- the daemon missing;
- the requested switch.

**QML tests:**

- the Settings page;
- quick settings' list;
- the dialog and inline questions for every kind, with Enter, Esc, focus and
  accessible names.

**By hand** (recorded in [hardware](hardware.md)), in a Vela session with
only `bluez`:

- an LE mouse, a keyboard (passkey or legacy PIN), a phone (numeric
  comparison, from both sides), a headset (battery, codec link) and a Swift
  Pair device;
- unplugging a USB dongle;
- `systemctl restart bluetooth` with Settings open;
- `systemctl disable --now bluetooth`, then the fix button;
- the hardware switch;
- a Plasma session on another console at the same time.

## 13. Stages

| Stage | Content | |
|---|---|---|
| B0 | Remove BlueDevil (§11) | |
| B1 | `vela-bluetooth`: start by the compositor, model, operations, errors, the `org.vela.Bluetooth` API settled; the fake BlueZ and the daemon's tests | |
| B2 | The agent (§6.4) and the questions, with tests | |
| B3 | `common/bluetooth`; the Settings page rewritten (§10.1); the old Settings code deleted | |
| B4 | The shell: `SystemStatus` on the client library, quick settings' list (§10.2), the question dialog and notifications (§10.3); `vela-audio` batteries from the daemon | |
| B5 | Search leases and discoverability (§6.5), nearby devices (§6.6) | |
| B6 | Diagnostics and fixes (§9) | |
| B7 | Docs and packaging: installation, hardware, testing | |
| B8 | The user tries it on real devices (§12, "by hand") | |

## 14. Open questions and risks

- **To decide: experimental BlueZ for nearby devices** (§6.6). No
  supported distribution starts `bluetoothd` with `Experimental`.
  `main.conf` has no drop-in directory. The options:
  1. Vela's package adds a systemd drop-in to `bluetooth.service` that
     passes `-E`. This changes every desktop on the machine and turns on
     all of BlueZ's experimental interfaces, not only the monitor.
  2. Settings offers to turn it on, as an explicit choice authorized by
     polkit. It writes that drop-in and restarts `bluetoothd`.
  3. Vela doesn't touch BlueZ's configuration. The feature works wherever
     the interface exists with offload, and is hidden elsewhere.

  Which controllers report `controller-patterns` on Linux (the kernel's
  MSFT extension) also has to be measured on real hardware.
- **`0000` refused.** A legacy device that wants another fixed PIN fails
  once. The failure could offer "Try with a PIN". Proposed: add it only if
  B8 finds such a device.
- **Default agent with two desktops.** BlueZ has one default agent for the
  whole system. The last session to ask for it wins. Vela asks again each
  time it becomes active; Plasma may not.
- **Daemon restart during a pairing.** BlueZ watches the caller of `Pair`.
  If the caller leaves the bus, BlueZ cancels the authentication and
  disconnects the device (`create_bond_req_exit` in `src/device.c`). After
  a crash, the user sees the operation fail, not hang.
