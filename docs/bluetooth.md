# Bluetooth

Design of Vela's Bluetooth, rewritten from scratch: devices, pairing, quick
settings, and how Vela shares Bluetooth with other desktop sessions. This
document comes before the code: decisions are made here, and the code
follows them.

> **Status:** design under discussion in PR #146. No code yet. Everything in
> §3 is in scope for beta 0.1. Stages are listed in §12.

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
5. **Other desktop sessions are left alone.** Vela acts only while its own
   session is in front. It never changes adapter-wide settings or BlueZ's
   configuration on its own (§6.6).
6. **It costs nothing while idle.** Nothing scans unless the user is looking
   for a device.
7. **It can be tested without a radio.** Every automated test runs against a
   fake BlueZ on a private bus.

## 3. Scope

In beta 0.1:

- Pairing with Vela's own agent, for every pairing method (§6.4).
- Connecting, disconnecting, removing and renaming devices. Each device
  shows its type and its battery level, if it reports one.
- The PC is discoverable only while "Add a device" is open (§6.5).
- Quick settings: a Bluetooth device list with connect and disconnect
  (§9.2).
- Codec and profile of headsets: they already exist in `vela-audio`
  ([audio](audio.md) §6.2). The Bluetooth page links to them (§9.1).
- Robustness: `bluetoothd` restarts, adapters plugged in and out, several
  adapters.

Decided out:

- **Battery notifications.** Battery levels are shown, but low battery is
  never notified.
- **File transfer (OBEX)**, in either direction. "Send via Bluetooth" goes
  away with BlueDevil (§10).

Not discussed yet, so not in 0.1: tethering (PAN), and any UI specific to
LE Audio. Whatever PipeWire supports still plays.

## 4. Decisions taken

| Decision | Choice | Consequence |
|---|---|---|
| Process | **`vela-bluetooth`**, a session daemon started and supervised by the compositor, like `vela-polkit-agent` | pairings survive Settings and the shell; it is in the compositor's logind session (§6.1) |
| Language and libraries | **C (C17), with sd-bus and sd-event from `libsystemd`**, like `vela-brightness`. No Qt and no GLib | a process that runs all day links only libc and `libsystemd`. `sd_bus_track` follows who owns a search lease or an Add (§6.5, §7). `sd_bus_add_object_manager` serves the agent |
| Who talks to BlueZ | **only `vela-bluetooth`** | one model, one agent, one place for errors |
| Client API | **`org.vela.Bluetooth`** on the session bus (§7) | Settings, the shell and `vela-audio` are clients |
| Client library | **`common/bluetooth`**: a C++ model and a QML module | no client parses D-Bus by hand (§8) |
| Agent | in the daemon, with capability **`KeyboardDisplay`**, registered and made default **only while the session is active** | every pairing method works. BlueZ asks the agent of `Pair`'s own caller first (`agent_get(sender)` in `src/device.c`). When Vela leaves, the previous default agent is default again (§6.6) |
| Where questions appear | **inline in Settings** if Settings started the operation and is open; otherwise a **shell dialog**; requests the user did not start appear first as a **notification** | the user answers where they are looking (§9.3) |
| Discoverable | **only while "Add a device" is open**, through BlueZ's per-client `Discoverable` discovery filter (§6.5) | the PC is not visible all day, and Vela never writes the adapter's `Discoverable` property |
| Codec and profile | **stay in `vela-audio`**: PipeWire owns them | the daemon never touches audio |
| Battery in `vela-audio` | read **from `vela-bluetooth`** instead of BlueZ | one Bluetooth client |
| BlueDevil | **removed** | §10 |

## 5. Overview

```
bluetoothd (system bus)                  logind (session Active)
      ▲  ObjectManager, Adapter1, Device1, Battery1, AgentManager1
      │  ◄── Agent1 (served by vela-bluetooth)
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
- Without `org.bluez` on the bus, it stays running with `Available` false.
  When BlueZ appears, it starts working.

### 6.2 The model

- At start, and whenever `org.bluez` reappears, the daemon reads
  `GetManagedObjects` asynchronously. From then on it follows
  `InterfacesAdded`, `InterfacesRemoved` and `PropertiesChanged`.
- When `org.bluez` leaves the bus, adapters and devices are cleared at once.
  Operations fail and questions close (§6.3, §6.4).
- **`Available`** is true while BlueZ is on the bus with at least one
  adapter.
- **The adapter in use:**
  - It is the first adapter in BlueZ's object tree, unless the user chose
    another one.
  - If it disappears, the next adapter is used.
  - The choice is remembered by address in Vela's own configuration, so a
    USB dongle that comes back is picked again.
- **Devices** are those of the adapter in use. Each one has:
  - path, address, name (the `Alias`) and type (from `Icon` and `Class`);
  - paired, trusted, connected and blocked;
  - battery level (`Battery1`), or none;
  - signal strength (only while searching);
  - its operation and its open question, if any.
- **The adapter** has:
  - name;
  - powered;
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
| **Rename** | `Set Alias` on the device | |
| **Power** | `Set Powered` on the adapter | before turning on, the soft rfkill block is removed, as today (`shell/src/bluetoothpower.h`) |

- Every step has a D-Bus timeout longer than BlueZ's own, so BlueZ's error
  normally arrives first. BlueZ gives an agent 60 seconds per question
  (`REQUEST_TIMEOUT` in `src/agent.c`). `Pair` gets 90 seconds; the other
  steps get 30.
- **An operation ends early** when its device's `Device1` disappears, the
  adapter stops being powered, `org.bluez` leaves the bus, or the session
  stops being active (§6.6). Each case has its own message.
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

The agent is served at `/org/vela/BluetoothAgent` on the system bus, with
capability `KeyboardDisplay`. When it is registered, and made default,
follows the session (§6.6).

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
| `Release()` | — | the daemon registers again when BlueZ comes back, if the session is active |

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
- **Order:** one open question per device. Questions for different devices
  wait in a queue.

### 6.5 Searching and being discoverable

- Clients don't call `StartDiscovery` themselves. They take a **search
  lease** with `StartSearch(discoverable)`, which returns an id.
- The lease ends with `StopSearch(id)`, or when its owner leaves the bus.
- While at least one lease is held, the daemon has its own BlueZ discovery
  session. If any lease asked for `discoverable`, the daemon sets the
  discovery filter `{"Discoverable": true}` before `StartDiscovery`.
  "Add a device" asks for it.
- **This is the only way Vela makes the PC discoverable.** BlueZ handles
  the filter per client (`set_discovery_discoverable` in `src/adapter.c`):
  - it turns discoverability on only if the adapter isn't discoverable
    already;
  - it turns it off only if it turned it on itself, when the last
    discovering client stops or leaves the bus.

  So a daemon crash never leaves the PC visible, and a desktop that made
  the PC discoverable by itself is not overruled.
- **Add** suspends discovery while it pairs, because discovery disturbs
  pairing. It resumes it afterwards if leases are still held.

### 6.6 Other desktop sessions

BlueZ is one service for the whole machine. A Plasma or GNOME session on
another console, or another user's session, uses the same adapter and
devices. Vela follows three rules.

**1. Vela acts only while its session is active.** The daemon follows the
`Active` property of its logind session (found as in
[polkit agent](polkit-agent.md) §4.1).

- When the session becomes active, the daemon registers its agent
  (`RegisterAgent`) and asks to be the default (`RequestDefaultAgent`).
- When the session stops being active (the user switched to another
  console or user), the daemon:
  - cancels a running Add and closes its questions;
  - suspends its discovery session, so BlueZ also drops the
    discoverability it turned on for Vela (§6.5);
  - unregisters its agent (`UnregisterAgent`).
- BlueZ keeps default agents as a stack (`add_default_agent` and
  `remove_default_agent` in `src/agent.c`). Removing Vela's agent makes the
  previous default agent, such as Plasma's, the default again. Requests on
  the other console go to that desktop, never to an inactive Vela.
- While inactive, the daemon only keeps its model up to date. Reading
  BlueZ changes nothing for anyone.

**2. Vela never changes adapter-wide settings on its own.** It never
writes these:

- the adapter's `Discoverable`, `DiscoverableTimeout`, `Pairable`,
  `PairableTimeout` or `Alias`;
- BlueZ's configuration files;
- the state of `bluetooth.service`.

Discoverability only goes through the per-client filter (§6.5). Discovery
sessions are per client in BlueZ, so Vela's `StopDiscovery` never stops
another desktop's search.

**3. What does change the whole machine happens only on the user's
explicit action in Vela:**

- turning Bluetooth on or off, including the soft rfkill unblock;
- pairing, which also marks the device trusted;
- renaming and removing a device.

BlueZ shares these with every desktop by design. Vela doesn't hide that,
and doesn't do any of it in the background.

## 7. `org.vela.Bluetooth`

Served at `/org/vela/Bluetooth` on the session bus. This sketch is not
final; the exact signatures are settled in B1.

```
properties
  Available       b          BlueZ running with an adapter (§6.2)
  Active          b          the session is active (§6.6)
  Adapters        aa{sv}     address, name, path, in use
  Adapter         a{sv}      the adapter in use (§6.2)
methods
  Devices() → a{oa{sv}}
  SetPowered(b on)
  UseAdapter(s address)
  StartSearch(b discoverable) → u   lease (§6.5)
  StopSearch(u lease)
  Add(o device, b promptsHere)
  Connect(o device) / Disconnect(o device) / Remove(o device)
  Rename(o device, s name)
  Cancel(o device)
  Answer(u question, b accept, s value)
signals
  DevicesChanged(a{oa{sv}} changed, ao removed)
  OperationChanged(o device, s state, s error, s message)
  Question(u id, o device, s kind, s code, b unsolicited)
  QuestionClosed(u id)
```

- `promptsHere = true` means that the caller draws the questions for this
  operation (§9.3). If the caller leaves the bus while a question is open,
  the question moves to the shell.
- While `Active` is false, every method that would change something
  returns `org.vela.Bluetooth.Error.Inactive`.

## 8. `common/bluetooth`

A client library, like `common/audio` for sound:

- a C++ model of adapters and devices, fed by `DevicesChanged`. This is
  the only C++ part: it lives in Qt processes, like `common/audio`;
- a QML module, `Vela.Bluetooth`, with `available`, `devices`, `paired`,
  `found` and the actions;
- the `requested.h` behaviour for switches: shown at once, and returned to
  BlueZ's state if BlueZ refuses or never does it.

Users: `vela-shell` (quick settings, the question dialog and notifications,
`SystemStatus` for airplane mode), `vela-settings`, and `vela-audio` for
headset batteries. If the daemon is missing, the model is empty and
`available` is false.

## 9. UI

### 9.1 Settings

The Bluetooth page is rewritten on `Vela.Bluetooth`. The current
`settings/src/bluetooth.*` is deleted.

- At the top: the switch. When Bluetooth is not available, the page says
  "Bluetooth isn't available on this PC" instead.
- **Paired devices**, connected first. Each one shows:
  - its icon, name and state ("Connected", "Paired", "Connecting...",
    "Pairing...", "Waiting for you...", or the last error);
  - its battery level;
  - a menu with Connect or Disconnect, Rename and Remove;
  - for headsets, "Sound settings", which opens the audio device page with
    the codec and profile.
- **Add a device** opens a dialog. It takes a discoverable search lease
  (shown as "Discoverable as *name*"). It lists the devices found, nearest
  first. Clicking one starts an Add. Its questions replace the list in the
  same dialog. On success the dialog closes.
- **Advanced:** the adapter, if there are two or more.

### 9.2 Quick settings

- The Bluetooth tile gets an arrow, as Wi-Fi has. It opens a list of paired
  devices with their battery level and connect or disconnect, plus "More
  Bluetooth settings".
- Unpaired devices are not listed there.

### 9.3 Questions from the agent

- **Settings started the operation and is open:** the question appears
  inline (§9.1).
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

## 10. Removing BlueDevil

- Files: the "Bluetooth" entry in Share (`shell/qml/FileDialogs.qml`), and
  `FileActions::canBluetooth` and `sendByBluetooth`
  (`shell/src/fileactions.*`).
- Packaging: the `bluedevil` optional dependency in `packaging/arch/PKGBUILD`.
- Docs: every mention.

`bluez` stays as today: `Recommends` on Fedora and an optional dependency on
Arch. [installation](installation.md) says it is all Bluetooth needs.

## 11. Tests

**Fake BlueZ.** This is a shared helper in `tests/harness/`, grown out of
`FakeServices` in `shell/tests/requested_switches.cpp`. It runs on a private
system bus, in its own thread. It serves:

- `ObjectManager`, `AgentManager1` (with BlueZ's default-agent stack),
  `Adapter1`, `Device1` and `Battery1`;
- per-client discovery sessions and the `Discoverable` filter, as in §6.5;
- a fake logind session with `Active`.

It records every property write on the adapter.

Each fake device follows a script:

- its pairing method: Just Works, confirmation, passkey entry, passkey
  display, or legacy PIN with the PIN it accepts;
- whether `Connect` succeeds;
- its delays.

On `Pair`, the fake calls the registered agent as BlueZ does. It can also:

- leave the bus and come back;
- remove a device or the adapter in the middle of an operation;
- send `Cancel` and `Release`;
- refuse any call.

**Daemon tests** (`bluetooth-*`). They run the real `vela-bluetooth` binary
against the fake. The pure parts (the operation states, the legacy PIN
rule, error mapping) also have plain C unit tests. The fake itself can stay
in Qt, because it is test code. Cases:

- the model, and rebuilding it after a restart;
- the adapter fallback, and remembering the chosen adapter;
- each row of §6.3, and each early ending;
- each row of §6.4, and the legacy PIN rule;
- search leases ending when their owner leaves, and the `Discoverable`
  filter set only for discoverable leases;
- **other sessions (§6.6):**
  - a second fake agent stands for another desktop;
  - when the session turns inactive, Vela's agent is unregistered and the
    other one is default again;
  - a running Add is cancelled, and discovery stops;
  - nothing changes while inactive;
  - Vela is registered and default again when the session is active;
- the fake's record shows no write to `Discoverable`,
  `DiscoverableTimeout`, `Pairable`, `PairableTimeout` or the adapter's
  `Alias`, in any test;
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
  comparison, from both sides) and a headset (battery, codec link);
- unplugging a USB dongle;
- `systemctl restart bluetooth` with Settings open;
- a Plasma session on another console at the same time:
  - pairing from Plasma while Vela is in the background works with
    Plasma's agent;
  - Plasma's discoverability is not changed by Vela;
  - switching back to Vela gives Vela the agent again.

## 12. Stages

| Stage | Content | |
|---|---|---|
| B0 | Remove BlueDevil (§10) | |
| B1 | `vela-bluetooth`: start by the compositor, model, operations, errors, the `org.vela.Bluetooth` API settled; the fake BlueZ and the daemon's tests | |
| B2 | The agent (§6.4), the questions, and the session rules (§6.6), with tests | |
| B3 | `common/bluetooth`; the Settings page rewritten (§9.1); the old Settings code deleted | |
| B4 | The shell: `SystemStatus` on the client library, quick settings' list (§9.2), the question dialog and notifications (§9.3); `vela-audio` batteries from the daemon | |
| B5 | Search leases and discoverability (§6.5) | |
| B6 | Docs and packaging: installation, hardware, testing | |
| B7 | The user tries it on real devices (§11, "by hand") | |

## 13. Open questions and risks

- **`0000` refused.** A legacy device that wants another fixed PIN fails
  once. The failure could offer "Try with a PIN". Proposed: add it only if
  B7 finds such a device.
- **Two active sessions at once** (several seats). Each one would push its
  own default agent, and the last one wins. Several seats are not
  supported in 0.1.
- **Daemon restart during a pairing.** BlueZ watches the caller of `Pair`.
  If the caller leaves the bus, BlueZ cancels the authentication and
  disconnects the device (`create_bond_req_exit` in `src/device.c`). After
  a crash, the user sees the operation fail, not hang.
