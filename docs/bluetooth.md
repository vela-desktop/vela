# Bluetooth

Design of Vela's Bluetooth, rewritten from scratch: devices, pairing, quick
settings, and how Vela shares Bluetooth with other desktop sessions. This
document comes before the code: decisions are made here, and the code
follows them.

> **Status:** B0–B3 are implemented in PR #146: the daemon and agent run,
> and Settings uses the shared client library. Shell, audio, search leases,
> lock-state propagation, packaging and hardware validation remain B4–B7.
> Everything in §3 is planned for beta 0.1. See §12.
> Until B4 supplies the shell's question presenter, unsolicited pairing and
> service authorization requests are immediately refused unless a trusted UI
> explicitly calls `PresentQuestions`. Settings does not opt in: only its own
> Add questions appear there. B5 will additionally propagate lock state.

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
  - To turn Bluetooth on, it removes the soft rfkill block of **every**
    Bluetooth radio (`RFKILL_OP_CHANGE_ALL` in `shell/src/bluetoothpower.h`),
    not only the adapter's.
- **`vela-audio`** reads Bluetooth batteries from BlueZ by itself.
- **Files** shares through `bluedevil-sendfile`, a KDE program.

## 2. Goals

1. **Bluetooth works with only `bluez` installed.** No KDE or GNOME
   component is needed. BlueDevil is removed.
2. **It never says yes on the user's behalf.** A device the user did not ask
   for cannot pair or use a service without a visible "Allow". No other app
   can answer in the user's place through Vela (§7.3).
3. **Operations outlive windows.** A pairing started in Settings finishes even
   if Settings is closed, or the shell restarts.
4. **Nothing gets stuck.** Every operation ends in "done", "failed" (in plain
   words) or "cancelled". This holds when the device disappears, the adapter
   is unplugged or `bluetoothd` restarts.
5. **Other desktop sessions are left alone.** Vela acts only while its own
   session is in front. It never changes adapter-wide settings or BlueZ's
   configuration on its own (§6.7).
6. **The PC is visible only as long as the user expects.** Visibility has a
   time limit, ends with the lock screen, and is always shown as it really
   is (§6.6).
7. **It costs nothing while idle.** Nothing scans unless the user is looking
   for a device.
8. **It can be tested without a radio.** Every automated test runs against a
   fake BlueZ on a private bus.

## 3. Scope

In beta 0.1:

- Pairing with Vela's own agent, for every pairing method (§6.4).
- Connecting, disconnecting, removing and renaming devices. Each device
  shows its type and its battery level, if it reports one.
- The PC is discoverable only while "Add a device" is open (§6.6).
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
| Language and libraries | **C (C17), with sd-bus and sd-event from `libsystemd`**, like `vela-brightness`. No Qt and no GLib | a process that runs all day links only libc and `libsystemd`. `sd_bus_track` follows who owns a search lease or an Add (§6.6, §7). `sd_bus_add_object_manager` serves the agent |
| Who talks to BlueZ | **only `vela-bluetooth`** | one model, one agent, one place for errors |
| Client API | **`org.vela.Bluetooth`** on the session bus (§7) | Settings, the shell and `vela-audio` are clients |
| Client library | **`common/bluetooth`**: a C++ model and a QML module | no client parses D-Bus by hand (§8) |
| Agent | in the daemon, with capability **`KeyboardDisplay`**, registered and made default **only while the session is active** | every pairing method works. BlueZ asks the agent of `Pair`'s own caller first (`agent_get(sender)` in `src/device.c`). When Vela leaves, the previous default agent is default again (§6.7) |
| Pairing concurrency | **one pairing at a time**, as BlueZ allows one open request per agent (§6.5) | a second Add is refused at once with the device that is pairing, instead of failing later inside BlueZ |
| Where questions appear | **inline in Settings** if Settings started the operation and is open; otherwise a **shell dialog**; requests the user did not start appear first as a **notification** | the user answers where they are looking (§9.3) |
| Who may answer | **only Vela's own UI** (`vela-shell`, `vela-settings`), checked by executable for every call that sees or answers a question (§7.3) | another app can't approve a pairing through Vela, or read its codes |
| Discoverable | **only while "Add a device" is open, for at most 3 minutes**, through BlueZ's per-client `Discoverable` discovery filter (§6.6) | Vela never writes the adapter's `Discoverable` property on its own |
| rfkill | **only the adapter's own rfkill switch** is unblocked, and only when the user turns Bluetooth on (§6.3) | other radios and adapters keep their state |
| Codec and profile | **stay in `vela-audio`**: PipeWire owns them | the daemon never touches audio |
| Battery in `vela-audio` | read **from `vela-bluetooth`** instead of BlueZ | one Bluetooth client |
| BlueDevil | **removed** | §10 |

## 5. Overview

```
bluetoothd (system bus)            logind (session Active, LockedHint)
      ▲  ObjectManager, Adapter1, Device1, Battery1, AgentManager1
      │  ◄── Agent1 (served by vela-bluetooth)
      ▼
vela-bluetooth (child of vela-compositor, no GUI) ── /dev/rfkill, /sys/class/bluetooth
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
  - `Discoverable` and `Discovering`, as BlueZ reports them.

### 6.3 Operations

| Operation | Steps | Notes |
|---|---|---|
| **Add** | `Pair` → `Set Trusted` → `Connect` | `AlreadyExists` counts as paired. If `Trusted` fails, the failure is shown but the operation goes on. If `Connect` fails, the device stays paired with "Paired, but couldn't connect" |
| **Connect** | `Connect` | |
| **Disconnect** | `Disconnect` | |
| **Remove** | `Adapter1.RemoveDevice` | ends when `Device1` disappears |
| **Rename** | `Set Alias` on the device | |
| **Power** | `Set Powered` on the adapter | turning on first unblocks the adapter's own rfkill switch (below) |

- Every step has a D-Bus timeout longer than BlueZ's own, so BlueZ's error
  normally arrives first. BlueZ gives an agent 60 seconds per question
  (`REQUEST_TIMEOUT` in `src/agent.c`). `Pair` gets 90 seconds; the other
  steps get 30.
- **An operation ends early** when:
  - its device's `Device1` disappears;
  - the adapter stops being powered;
  - `org.bluez` leaves the bus;
  - the session stops being in front (§6.7).

  Each case has its own message.
- **Cancel:**
  - while pairing, the daemon calls `Device1.CancelPairing` and closes the
    open question;
  - while connecting, it calls `Disconnect`;
  - while an Add sets `Trusted`, the Add stops there;
  - Disconnect, Remove and Rename can't be cancelled: they run to their
    end.
- **Errors:** BlueZ's error name and message go to the daemon's log. The
  client gets a short, stable code and a plain-words message, for example:
  - "The device isn't responding. Make sure it's on and nearby.";
  - "Pairing was refused on the device.";
  - "Bluetooth is busy. Try again in a moment.";
  - "Bluetooth isn't running."

  The user's own Cancel gives no message.

**rfkill.** The kernel registers one rfkill switch per Bluetooth adapter.
Its parent is the adapter: `/sys/class/bluetooth/hciN/rfkillM`, whose
`index` file gives its number. Some laptops also have platform switches of
type Bluetooth with no adapter behind them; `asus-wmi` and `hp-wmi`
register such switches.

- Turning on with an adapter: if the adapter's own switch is soft-blocked
  and not hard-blocked, the daemon writes `RFKILL_OP_CHANGE` with that
  index to `/dev/rfkill`. Then it sets `Powered`. This keeps today's retry
  while BlueZ notices the unblock (`bluetoothpower.h`). No other switch is
  touched.
- Turning on with no adapter at all, while a platform Bluetooth switch is
  soft-blocked: the built-in radio may be gone because of that switch. The
  switch in Settings and quick settings shows Off. Turning it on unblocks
  only the soft-blocked platform switches of type Bluetooth. Once an
  adapter appears, the first case applies.
- Turning off only sets `Powered` to false. Vela never sets an rfkill
  block.
- A hard block can't be removed from software. The switch shows Off and
  BlueZ's refusal turns it back, as today (`requested.h`).

`VELA_RFKILL` and `VELA_SYSFS` replace `/dev/rfkill` and `/sys` in the tests.
Only the test build of the daemon (`vela-bluetooth-testing`, never
installed) reads them; the shell's own unblock (B0) reads them too, until B4
removes it.

**Without an adapter**, the property `PlatformBlocked` (§7.1) is true while a
platform switch of type Bluetooth is blocked: the switch shows Off and can
be turned on. After it unblocks a platform switch, the daemon turns on the
adapter that appears within 10 seconds.

### 6.4 The agent

The agent is served at `/org/vela/BluetoothAgent` on the system bus, with
capability `KeyboardDisplay`. When it is registered, and made default,
follows the session (§6.7). Every `Agent1` call becomes a **question**
(§7.2).

| BlueZ call | Kind | What the user sees |
|---|---|---|
| `RequestConfirmation(device, passkey)` | `confirm-passkey` | "Does *Name* show 123456?" Pair / Cancel |
| `RequestAuthorization(device)` | `authorize-pairing` | "*Name* wants to pair" Allow / Deny |
| `AuthorizeService(device, uuid)` | `authorize-service` | "Allow *Name* to use *service*?" Allow / Deny |
| `RequestPasskey(device)` | `enter-passkey` | a 6-digit field |
| `RequestPinCode(device)` | `enter-pin` | a text field, unless the legacy rule applies (below) |
| `DisplayPasskey(device, passkey, entered)` | `show-passkey` | "Type 123456 on *Name*, then Enter", with the digits typed so far |
| `DisplayPinCode(device, pincode)` | `show-pin` | "Type 0000 on *Name*, then Enter" |
| `Cancel()` | — | the open question closes with reason `cancelled-by-device` |
| `Release()` | — | the daemon registers again when BlueZ comes back, if the session is active |

- **Legacy PIN** (`RequestPinCode`), only for an Add the user started:
  - a keyboard (`Icon` `input-keyboard`) gets a random 6-digit PIN, shown
    as a `show-pin` question;
  - mice, gamepads and audio devices get `0000`;
  - every other device, and every request the user did not start: the user
    types the PIN.
- Nothing else is answered without the user. BlueZ authorizes services of
  trusted devices by itself, without the agent (`src/adapter.c`). Devices
  Vela pairs are trusted, so `AuthorizeService` only comes from devices
  paired elsewhere, or from devices the user marked as not trusted.
- **Shown codes.** BlueZ expects no reply to `DisplayPasskey`, and calls it
  again as keys are typed on the device: the question stays the same, and
  `QuestionChanged` gives the digits typed so far. `DisplayPinCode` is
  answered at once, because BlueZ gives the PIN to the kernel only then.
  So `show-passkey` and `show-pin` stay open until the device is paired,
  the Add's `Pair` ends, or 60 seconds pass.
- **Refused at once** (`org.bluez.Error.Rejected`), and logged: a call to
  the agent that doesn't come from BlueZ, a request about a device of
  another adapter than the one in use, any request while the session is
  inactive, and a request while another question is open.
- **While the screen is locked** (`LockedHint`, §6.7), requests the user did
  not start are rejected at once (`org.bluez.Error.Rejected`). Nobody is
  there to answer them.

### 6.5 Concurrent requests

BlueZ gives each agent **at most one open request**. A second request, for
another device or of another kind, fails at once with `-EBUSY`
(`agent_has_request` and `agent_request_pincode` in `src/agent.c`). The
pairing or service request that caused it is then refused. Vela's rules
follow from this:

1. **One Add at a time.** An Add for another device while one runs is
   refused with `org.vela.Bluetooth.Error.Busy`, naming the device that is
   pairing. Settings disables the other devices in "Add a device" and says
   "Wait for *Name* to finish pairing".
2. **The same device joins.** A second Add for the device that is pairing
   joins the running one, and both callers get its result.
3. **An open unsolicited question blocks other Adds.** If a phone is asking
   to pair, an Add for any other device is refused with `Busy` until that
   question closes. BlueZ would refuse it anyway.
4. **Add on the device that is asking.** If a device has an open
   unsolicited question and the user clicks Add on that same device, the
   daemon does not call `Pair`. The Add adopts the question: it becomes
   solicited, and moves inline into Settings if Settings asked for that.
   After the user accepts, the Add waits for `Paired`, then sets `Trusted`
   and connects.
5. **Requests arriving while a question is open** are refused by BlueZ
   before Vela sees them. The daemon logs what it can. It never queues
   questions, because BlueZ never sends a second one.
6. **Other operations** run in parallel on different devices. On a device
   with an Add running, everything but `Cancel` is refused with `Busy`.
7. **Races on one question:**
   - the first `Answer` wins, and a later one gets `QuestionClosed`;
   - an `Answer` that arrives after BlueZ's `Cancel` gets `QuestionClosed`;
   - a `Cancel` from BlueZ after Vela has replied is ignored.

### 6.6 Searching and being discoverable

**Search leases.**

- Clients don't call `StartDiscovery` themselves. They take a lease with
  `StartSearch(discoverable)`, which returns an id.
- While at least one lease is held, the daemon keeps one BlueZ discovery
  session of its own.
- A lease ends at the first of:
  - `StopSearch(id)`;
  - its owner leaving the bus (`sd_bus_track`);
  - **3 minutes** after it started;
  - the screen locking, or the session stopping being active (§6.7);
  - the adapter turning off, or going away.

  Ending a lease is final. To search again, the client asks for a new
  lease; Settings shows "Search again" for this.
- **Add** suspends discovery while it pairs, because discovery disturbs
  pairing. Discovery resumes after the Add if leases are still held and
  still within their time.
- If BlueZ stops discovering by itself while leases are held (an adapter
  reset, for example), the daemon starts again, at most three times a
  minute.

**How BlueZ handles visibility.** These points are checked in
`src/adapter.c`, BlueZ 5.86:

- The discovery filter `{"Discoverable": true}` makes the adapter
  discoverable while that client discovers. If the adapter is already
  discoverable, the filter does nothing (`set_discovery_discoverable`).
- BlueZ turns that visibility off only when the **last** discovering client
  stops or leaves the bus (`discovery_stop`). If another client, such as
  another desktop's search or a command-line tool, is still discovering,
  the adapter **stays discoverable with no timeout**.
- `DiscoverableTimeout` defaults to 0, meaning no limit (`main.conf`).
  Writing it is stored for the adapter and would change it for everyone,
  so Vela never does.

**Vela's rules for visibility.**

1. **A discoverable lease uses the filter only when Vela would be the only
   searcher.** At the lease's start the daemon reads `Discovering`.
   - If it is false, the daemon calls `SetDiscoveryFilter({"Discoverable":
     true})` and then `StartDiscovery`.
   - If another client is already discovering, the daemon searches without
     the filter. "Add a device" then says: "Another app is searching for
     devices, so this PC isn't made visible. Pairing from this PC still
     works."
2. **What the UI shows is BlueZ's `Discoverable`, never the lease.**
   "Discoverable as *name*" appears only while the adapter really is
   discoverable.
3. **When visibility outlives Vela's search.** Another client may start
   discovering during Vela's lease. When Vela's lease ends, the daemon then
   checks `Discoverable` two seconds later. If it is still true, and it was
   false before Vela's lease, Vela tells the user:
   - in Settings, while it is open: a line "Your PC is still visible to
     nearby devices, because another app is searching" with "Stop being
     visible";
   - otherwise, a notification with the same text and action.

   "Stop being visible" is an explicit user action (§6.7, rule 3). It
   writes `Discoverable` false on the adapter. Vela never does this on its
   own.

### 6.7 Other desktop sessions

BlueZ is one service for the whole machine. A Plasma or GNOME session on
another console, or another user's session, uses the same adapter and
devices. The daemon follows its logind session's `Active` and `LockedHint`
properties. The session is found as in [polkit agent](polkit-agent.md)
§4.1. Vela follows three rules.

**1. Vela acts only while its session is in front.**

- When the session becomes active, the daemon registers its agent
  (`RegisterAgent`) and asks to be the default (`RequestDefaultAgent`).
- When the session stops being active (the user switched to another
  console or user), the daemon:
  - cancels a running Add and closes its questions;
  - ends every search lease, which also drops the discoverability BlueZ
    turned on for Vela (§6.6);
  - unregisters its agent (`UnregisterAgent`).
- BlueZ keeps default agents as a stack (`add_default_agent` and
  `remove_default_agent` in `src/agent.c`). Removing Vela's agent makes the
  previous default agent, such as Plasma's, the default again. Requests on
  the other console go to that desktop, never to an inactive Vela.
- **While the screen is locked**, the agent stays registered, so requests
  for this seat don't fall to another desktop. But search leases end, a
  running Add is cancelled, and unsolicited requests are rejected (§6.4).
  - `LockedHint` is set by the compositor through logind's
    `Session.SetLockedHint` while the lock screen is up. **Vela doesn't do
    this today**; it is added in B5.
- While inactive, the daemon only keeps its model up to date. Reading
  BlueZ changes nothing for anyone. Methods that would change something
  return `org.vela.Bluetooth.Error.Inactive`; so they do while the screen
  is locked. `Cancel` always works.
- If logind can't name the session, `XDG_SESSION_ID` is used. With no
  session at all, or no logind, the daemon acts as if the session were
  always in front, as Bluetooth did before.

**2. Vela never changes adapter-wide settings on its own.** It never
writes these:

- the adapter's `Discoverable`, except on the user's "Stop being visible"
  (§6.6);
- the adapter's `DiscoverableTimeout`, `Pairable`, `PairableTimeout` or
  `Alias`;
- rfkill blocks;
- BlueZ's configuration files;
- the state of `bluetooth.service`.

Discovery sessions are per client in BlueZ, so Vela's `StopDiscovery`
never stops another desktop's search.

**3. Whole-machine changes happen only on the user's explicit action in
Vela:**

- turning Bluetooth on or off, with the rfkill unblock of §6.3;
- pairing, which also marks the device trusted;
- renaming and removing a device;
- "Stop being visible".

BlueZ shares these with every desktop by design. Vela doesn't hide that,
and doesn't do any of it in the background.

## 7. `org.vela.Bluetooth`

Served at `/org/vela/Bluetooth` on the session bus. Error names are under
`org.vela.Bluetooth.Error`. Device and property dictionaries may grow, and
clients ignore keys they don't know. Signatures are final once B1 is
merged.

B1 and B2 serve everything in §7.1 and §7.2 except the searches
(`StartSearch`, `StopSearch`, `StopBeingVisible`, `SearchEnded`), which come
with B5.

### 7.1 Devices, adapter and searches

```
properties
  Available       b          BlueZ running with an adapter (§6.2)
  PlatformBlocked b          no adapter, and a platform Bluetooth switch is blocked (§6.3)
  Active          b          the session is in front: active and not locked (§6.7)
  Adapters        aa{sv}     path, address, name, inUse
  Adapter         a{sv}      the adapter in use (§6.2): path, address, name,
                             powered, powerState, discoverable, discovering
methods
  Devices() → a{oa{sv}}
  SetPowered(b on)
  UseAdapter(s address)
  StartSearch(b discoverable) → t lease          (§6.6)
  StopSearch(t lease)
  StopBeingVisible()                              (§6.6, rule 3)
  Add(o device, b promptsHere)                    returns when the Add ends
  Connect(o device) / Disconnect(o device) / Remove(o device)
  Rename(o device, s name)
  Cancel(o device)
signals
  DevicesChanged(a{oa{sv}} changed, ao removed)
  OperationChanged(o device, s state, s error, s message)
  SearchEnded(t lease, s reason)                  stopped, owner-gone, time-limit,
                                                  locked, inactive, adapter-off
```

A device's dictionary has `address`, `name`, `icon`, `type` (mouse,
keyboard, gamepad, tablet, headset, headphones, speaker, phone, computer or
other), `paired`, `trusted`, `connected` and `blocked`; `battery` (`y`) and
`rssi` (`n`) when BlueZ reports them; `operation` (the running step:
`pairing`, `trusting`, `connecting`, `disconnecting`, `removing`,
`renaming`) while one runs; and `error` and `message` after an operation
that failed.

Methods that change something return when it is done, with an error if it
failed. On the way, `OperationChanged` gives each step's state, then
`done`, `failed` or `cancelled`, with the stable code and the message (§6.3).
It comes after the `DevicesChanged` that shows the device's new state. The
same operation asked again on the same device joins the running one.

Errors:

- `Busy`: the message is the object path of the device that is busy, or
  of the device that is pairing (§6.5);
- `Failed`: the message is the plain-words message (§6.3), and
  `OperationChanged` has the code;
- `Cancelled`, `UnknownDevice`, `UnknownAdapter`, `NotAllowed` (§7.3),
  `Inactive` (§6.7);
- `org.freedesktop.DBus.Error.InvalidArgs` for a name `Rename` refuses:
  not UTF-8, a control character, or longer than 248 bytes.

### 7.2 Questions

A question is one open `Agent1` request (§6.4). There is at most one at a
time, because BlueZ sends no more (§6.5).

```
methods
  Questions() → a(toa{sv})              the open question, if any: id, device, details
  Answer(t id, s response, s value)
signals
  Question(t id, o device, a{sv} details)
  QuestionChanged(t id, a{sv} details)  only `entered`, for show-passkey
  QuestionClosed(t id, s reason)
```

**Ids** are 64-bit and never reused while the daemon runs. When
`org.vela.Bluetooth` changes owner (the daemon restarted), clients drop
every question they hold.

**Receiving them.** A client of Vela's UI calls `Questions()` when it
starts, and again when the daemon restarts. From then on, while it stays
on the bus, it receives `Question`, `QuestionChanged` and
`QuestionClosed`. `Add(device, promptsHere = true)` does the same for its
caller.

**Details:**

| Key | Type | Present for |
|---|---|---|
| `kind` | `s` | always (§6.4) |
| `name`, `icon`, `address` | `s` | always, from the device at the time of the request |
| `unsolicited` | `b` | always: true when no Add of the user's is behind it |
| `drawnBy` | `s` | always: `settings` or `shell` (§9.3) |
| `expires` | `t` | always: `CLOCK_MONOTONIC` microseconds when BlueZ gives up (60 s after the request) |
| `passkey` | `u` | `confirm-passkey`, `show-passkey` (0–999999, shown with 6 digits) |
| `entered` | `q` | `show-passkey`: digits typed so far |
| `pin` | `s` | `show-pin` |
| `uuid`, `service` | `s` | `authorize-service`: the UUID and its name in plain words, or the UUID if unknown |

**Answers.** Each kind accepts only these responses:

| Kind | `response` | `value` | Reply to BlueZ |
|---|---|---|---|
| `confirm-passkey`, `authorize-pairing`, `authorize-service` | `accept` | empty | empty reply |
| same | `reject` | empty | `org.bluez.Error.Rejected` |
| `enter-passkey` | `accept` | decimal 0–999999 | the number |
| `enter-pin` | `accept` | 1–16 bytes of UTF-8, no control characters | the string |
| `enter-passkey`, `enter-pin`, `show-passkey`, `show-pin` | `cancel` | empty | `org.bluez.Error.Canceled`; for an Add, also `CancelPairing` |

`reject` and `cancel` both end the operation. Only the user's own choice
gives no error message (§6.3).

`Answer` errors:

- `UnknownQuestion`: no such id;
- `QuestionClosed`: it was already answered or cancelled (§6.5, rule 7);
- `WrongResponse`: the response is not valid for this kind;
- `InvalidValue`: the value is outside the rules above;
- `NotAllowed` (§7.3);
- `Inactive`.

**`QuestionClosed` reasons:**

| Reason | Meaning |
|---|---|
| `answered` | Accepted, rejected or cancelled. All clients close it, wherever it was answered |
| `cancelled-by-device` | BlueZ's `Cancel`, including its 60-second timeout |
| `device-gone` | The device disappeared |
| `bluetooth-gone` | `org.bluez` left the bus, or the adapter went away. BlueZ removes an adapter's devices first, so an unplugged dongle usually gives `device-gone` |
| `inactive` | The session stopped being active |
| `locked` | The screen locked |

**Who draws it.** `Add(device, promptsHere = true)` sets `drawnBy` to
`settings`. If that caller leaves the bus while the question is open, the
daemon sends `Question` again with the same id and `drawnBy` = `shell`.

### 7.3 Who may call

**What is protected.** BlueZ's D-Bus policy lets any local process call
`org.bluez` (`policy context="default"` in `src/bluetooth.conf`). So any
unsandboxed process of the user can already pair, connect and remove
devices with its own agent. `org.vela.Bluetooth` doesn't try to be
stricter than BlueZ for those actions. What Vela adds is the default
agent's answers: approving a pairing or a service that a remote device
asked for. Those answers, and the codes in the questions, are reserved
to Vela's own UI.

| Calls | Allowed callers |
|---|---|
| Properties, `Devices` | any process of the same user |
| `SetPowered`, `UseAdapter`, `StartSearch`, `StopSearch`, `StopBeingVisible`, `Add` with `promptsHere = false`, `Connect`, `Disconnect`, `Remove`, `Rename`, `Cancel` | same user, not sandboxed |
| `Add` with `promptsHere = true`, `Questions`, `Answer`; receiving `Question`, `QuestionChanged` and `QuestionClosed` | **Vela's UI only**: `vela-shell` and `vela-settings` |

- **Same user:** the caller's UID, from the bus credentials
  (`sd_bus_query_sender_creds`), equals the daemon's.
- **Not sandboxed:** the caller is not a Flatpak or a Snap. The checks are
  the ones `xdg-desktop-portal` uses: `/.flatpak-info` in the caller's root,
  or a `snap.` cgroup. Flatpak's D-Bus proxy normally hides the name
  anyway.
- **Vela's UI:** the caller's executable is one of the installed paths of
  `vela-shell` and `vela-settings`, fixed at build time. A daemon run from
  its build directory also trusts the `vela-shell` and `vela-settings`
  built next to it; an installed daemon never does.
  - The daemon gets the caller's pidfd from the bus
    (`sd_bus_creds_get_pidfd_dup`, the broker's `ProcessFD`) and reads
    `/proc/PID/exe`.
  - It then checks that the pidfd still refers to that process, so a
    reused PID can't pass.
  - Without a pidfd, it falls back to the PID, and only for this check.
    The pidfd needs systemd 256 or later and a bus that gives `ProcessFD`
    (dbus-broker does, and dbus-daemon 1.16 does).
  - A binary replaced by an update shows as "*path* (deleted)", and is
    still accepted: only root installs at that path.
  - The test build of the daemon has its own list: the tests' clients
    only (the daemon's tests and the client library's).
- **Signals carrying questions are sent to each allowed client only**
  (unicast, with a destination), never broadcast: to the callers of
  `Questions()` and of `Add` with `promptsHere` (§7.2).
- Refusals return `org.vela.Bluetooth.Error.NotAllowed` and are logged
  with the caller's executable.

**The limit:** this is not a wall against malicious code already running
as the user. Such code could talk to BlueZ directly, or tamper with Vela's
own processes. It stops other apps from answering pairing questions
through Vela, and from reading their codes.

## 8. `common/bluetooth`

A client library, like `common/audio` for sound:

- a C++ model of adapters and devices, fed by `DevicesChanged`. This is
  the only C++ part: it lives in Qt processes, like `common/audio`;
- the `Bluetooth` singleton of the QML module `Vela.Bluetooth`, which each
  process registers with its own instance. It has `available`, `devices`,
  `paired`, `found`, `question` and the actions;
- the `requested.h` behaviour for switches: shown at once, and returned to
  BlueZ's state if BlueZ refuses or never does it.

A refusal that isn't a device's own failure (`Busy`, `Inactive`, a name
`Rename` refuses...) is the model's `error`, in plain words. A device's
failure stays with the device, in its `error` and `message`. Until the
daemon serves search leases (B5), "Add a device" lists the devices BlueZ
already knows.

Users: `vela-shell` (quick settings, the question dialog and notifications,
`SystemStatus` for airplane mode), `vela-settings`, and `vela-audio` for
headset batteries. If the daemon is missing, the model is empty and
`available` is false.

## 9. UI

### 9.1 Settings

The Bluetooth page is rewritten on `Vela.Bluetooth`, and the old
`settings/src/bluetooth.*` is deleted. Settings' home uses the same model
for its Bluetooth card.

- At the top: the switch. When Bluetooth is not available, the page says
  "Bluetooth isn't available on this PC" instead.
- "Your PC is still visible to nearby devices" with "Stop being visible",
  when §6.6 rule 3 applies.
- **Paired devices**, connected first. Each one shows:
  - its icon, name and state ("Connected", "Paired", "Connecting...",
    "Pairing...", "Waiting for you...", or the last error);
  - its battery level;
  - a menu with Connect or Disconnect, Rename and Remove;
  - for headsets, "Sound settings", which opens the audio device page with
    the codec and profile.
- **Add a device** opens a dialog.
  - It takes a discoverable lease. It shows "Discoverable as *name*" only
    while BlueZ says so, or the "another app is searching" line (§6.6).
  - It lists the devices found, nearest first. While one pairs, the others
    are disabled (§6.5).
  - When the lease ends (3 minutes, lock, ...), the list stays and a
    "Search again" button appears.
  - Clicking a device starts an Add. Its questions replace the list in the
    same dialog. On success the dialog closes.
- **Advanced:** the adapter, if there are two or more.

### 9.2 Quick settings

- The Bluetooth tile gets an arrow, as Wi-Fi has. It opens a list of paired
  devices with their battery level and connect or disconnect, plus "More
  Bluetooth settings".
- Unpaired devices are not listed there.

### 9.3 Questions from the agent

- **`drawnBy` = `settings`:** inline in Settings (§9.1).
- **`drawnBy` = `shell`, solicited:** a shell dialog. It is a small window
  centered on the primary screen, styled like `ConfirmDialog`, not the
  full-screen polkit prompt.
  - It shows the device's icon and name, the code in large digits, and the
    field when one is needed.
  - Enter confirms and Esc cancels. The field has focus.
  - Every element has an accessible name. The code is read digit by digit.
  - The dialog closes by itself on `QuestionClosed`.
  - It shows the time left from `expires` once fewer than 15 seconds
    remain.
- **`drawnBy` = `shell`, unsolicited** (a phone or headset starting the
  pairing, or a service request): a notification first.
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

**Fake BlueZ.** This is `tests/bluetooth/fakebluez.h`, grown out of
`FakeServices` in `shell/tests/requested_switches.cpp`. It runs on a private
system bus, in its own thread. Discovery sessions and the `Discoverable`
filter come with B5; the rest of this list is there since B2. It serves:

- `ObjectManager`, `AgentManager1`, `Adapter1`, `Device1` and `Battery1`;
- BlueZ's default-agent stack, and one open request per agent, refusing
  others with `-EBUSY` as BlueZ does;
- per-client discovery sessions and the `Discoverable` filter. Visibility
  is undone only when the last client stops, as in §6.6;
- a fake logind session with `Active` and `LockedHint`.

It records every property write on the adapter. A fake `/dev/rfkill` and
`/sys` (`VELA_RFKILL`, `VELA_SYSFS`) hold several adapters and a platform
switch.

Each fake device follows a script:

- its pairing method: Just Works, confirmation, passkey entry, passkey
  display, or legacy PIN with the PIN it accepts;
- whether `Connect` succeeds;
- its delays;
- whether it starts a pairing by itself.

On `Pair`, the fake calls the registered agent as BlueZ does. It can also:

- leave the bus and come back;
- remove a device or the adapter in the middle of an operation;
- send `Cancel` and `Release`;
- refuse any call;
- run a second discovering client.

**Daemon tests** (`bluetooth-*`). They run the real `vela-bluetooth` binary
against the fake. The pure parts (the operation states, the legacy PIN
rule, error mapping, answer validation) also have plain C unit tests. The
fake itself can stay in Qt, because it is test code. Cases:

- the model, and rebuilding it after a restart;
- the adapter fallback, and remembering the chosen adapter;
- each row of §6.3, and each early ending;
- **rfkill:**
  - only the adapter's own switch is unblocked;
  - the other adapter, the Wi-Fi switch and a hard-blocked switch are
    untouched;
  - the platform switch is unblocked only when no adapter exists;
- each kind of §6.4, with each valid and invalid answer of §7.2, and the
  legacy PIN rule;
- **concurrency** (each rule of §6.5):
  - two Adds;
  - Add while a phone asks to pair;
  - Add on the device that is asking;
  - two `Answer`s;
  - `Answer` after `Cancel`;
- **questions:**
  - `drawnBy` moving to the shell when Settings leaves the bus;
  - `Questions()` after a client restart;
  - each `QuestionClosed` reason;
- **search leases:**
  - each way a lease ends, including the 3-minute limit and the lock;
  - the filter only when no other client discovers;
  - "still visible" reported when the second fake client keeps the adapter
    discoverable, and `StopBeingVisible` clearing it;
- **other sessions (§6.7):**
  - a second fake agent stands for another desktop;
  - when the session turns inactive, Vela's agent is unregistered and the
    other one is default again;
  - a running Add is cancelled, and discovery stops;
  - nothing changes while inactive;
  - Vela is registered and default again when the session is active;
- the fake's record shows no write to `Discoverable` (except through
  `StopBeingVisible`), `DiscoverableTimeout`, `Pairable`, `PairableTimeout`
  or the adapter's `Alias`, in any test;
- no operation, lease or question left open after any of the cases above.

**Authorization tests** (§7.3):

- `dbus-send`, an unknown executable, reads the devices and adds without
  `promptsHere`, but can't add with `promptsHere`; from B2, it can't
  answer or see questions either;
- the sandbox checks (`/.flatpak-info`, a `snap.` cgroup) are unit tests on
  a fake `/proc`;
- the test build's list lets the test client add with `promptsHere`, and
  from B2 answer;
- question signals reach only the allowed clients (B2).

**Client library tests:**

- the model against the daemon running on the fake;
- the daemon missing;
- the requested switch.

**QML tests** (Settings' with a fake `Vela.Bluetooth` written in QML):

- the Settings page, including "Search again" and "Stop being visible";
- quick settings' list;
- the dialog and inline questions for every kind, with Enter, Esc, focus,
  the time left and accessible names.

**By hand** (recorded in [hardware](hardware.md)), in a Vela session with
only `bluez`:

- an LE mouse, a keyboard (passkey or legacy PIN), a phone (numeric
  comparison, from both sides) and a headset (battery, codec link);
- a phone asking to pair while a mouse is pairing;
- unplugging a USB dongle, and two adapters at once;
- a laptop with a platform Bluetooth switch, blocked from Plasma;
- `systemctl restart bluetooth` with Settings open;
- `bluetoothctl scan on` running while "Add a device" is open, then
  closed;
- locking the screen with "Add a device" open;
- a Plasma session on another console at the same time:
  - pairing from Plasma while Vela is in the background works with
    Plasma's agent;
  - Plasma's discoverability is not changed by Vela;
  - switching back to Vela gives Vela the agent again.

## 12. Stages

| Stage | Content | |
|---|---|---|
| B0 | Remove BlueDevil (§10); the shell's rfkill unblock limited to the adapter's own switch, until B4 replaces it | done |
| B1 | `vela-bluetooth`: start by the compositor, model, operations, errors, rfkill (§6.3), the `org.vela.Bluetooth` API settled with its authorization (§7.3); the fake BlueZ and the daemon's tests | done |
| B2 | The agent (§6.4), concurrency (§6.5), the questions (§7.2), and the session rules (§6.7), with tests | done |
| B3 | `common/bluetooth`; the Settings page rewritten (§9.1); the old Settings code deleted | done |
| B4 | The shell: `SystemStatus` on the client library, quick settings' list (§9.2), the question dialog and notifications (§9.3); `vela-audio` batteries from the daemon | |
| B5 | Search leases and visibility (§6.6); the compositor sets `LockedHint` while the lock screen is up | |
| B6 | Docs and packaging: installation, hardware, testing | |
| B7 | The user tries it on real devices (§11, "by hand") | |

## 13. Open questions and risks

- **`0000` refused.** A legacy device that wants another fixed PIN fails
  once. The failure could offer "Try with a PIN". Proposed: add it only if
  B7 finds such a device.
- **Platform rfkill switches** (§6.3) behave differently between laptops:
  the adapter may disappear, or stay with `PowerState` `off-blocked`. B7
  must check at least one laptop with such a switch.
- **"Stop being visible" and other searchers.** Writing `Discoverable`
  false also clears BlueZ's note that a filter made the adapter visible
  ("Discoverable takes precedence", `property_set_discoverable` in
  `src/adapter.c`). If the other searcher had asked for visibility too, it
  loses it. BlueZ applies filters again only when a client starts or
  stops. Because this is an explicit user action (§6.7, rule 3), it is
  accepted. Vela always shows the real `Discoverable` (§6.6, rule 2).
- **Two active sessions at once** (several seats). Each one would push its
  own default agent, and the last one wins. Several seats are not
  supported in 0.1.
- **Daemon restart during a pairing.** BlueZ watches the caller of `Pair`.
  If the caller leaves the bus, BlueZ cancels the authentication and
  disconnects the device (`create_bond_req_exit` in `src/device.c`). After
  a crash, the user sees the operation fail, not hang.
