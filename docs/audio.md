# Audio

Design of Vela's audio backend: one native library, shared by the shell and
Settings. It replaces today's three backends, which drive command-line tools
(`wpctl`, `pactl`, `pw-dump`, `pw-metadata`). This document comes before the
code: decisions are made here, and the code follows them.

> **Status:** design under discussion. No code yet.

## 1. Goals

1. **One backend.** The system volume, the volume keys and their indicator,
   the quick settings mixer, the wheel on a taskbar button and every Sound
   page in Settings read and write the same model, through the same code.
2. **Native.** No processes are started to read or change anything. Changes
   arrive as events, in order, as soon as they happen. Nothing blocks the
   interface thread (today `SystemStatus::deviceIntervals()` waits up to
   1.5 s for `wpctl` and `pw-dump`).
3. **On par with Windows 11's sound settings**, minus the parts that have no
   common base on Linux (§10).
4. **WirePlumber stays in charge.** Vela shows and asks; the session manager
   decides and remembers (§3). Vela never keeps its own copy of the system's
   audio state.
5. **Tested for real.** Automated tests run against a real PipeWire and
   WirePlumber on fake devices, not against fake command-line tools (§11).

## 2. Decisions taken

| Decision | Choice | Consequence |
|---|---|---|
| Sound server | **PipeWire with WirePlumber only** | PulseAudio without PipeWire is not supported. `pipewire` and `wireplumber` become hard dependencies of the package; `libpulse` goes (§12) |
| Library | **libwireplumber 0.5** (the library `wpctl` is built on), with its `mixer-api` and `default-nodes-api` plugins | volume scale, device routes and defaults behave exactly as with `wpctl` today; libpulse is out of the question |
| Where it lives | a library, **`common/audio`**, linked by the shell and by Settings | each process has its own connection to PipeWire. PipeWire itself is the shared service: no Vela audio daemon |
| Event loop | WirePlumber's GLib sources on the **default `GMainContext`**, run by Qt's own loop (`QEventDispatcherGlib`), in the interface thread | no threads, no locks, no glue code (§4.1) |
| Maximum volume | **100%** everywhere | Settings no longer goes up to 150% (§5.3) |
| Vela's settings | WirePlumber's **runtime settings** where they exist; otherwise **one file owned by Vela**, `~/.config/wireplumber/wireplumber.conf.d/90-vela.conf` | changes from the file need a WirePlumber restart, with a short audio gap, said before applying (§8) |
| "Microphone in use" | **honest**: Settings' own level meter counts too | an indicator that hides someone, even us, can no longer be trusted (§7.2) |
| Test sound | **synthesized** in code, left channel then right | no sound file to ship, no decoder (§7.3) |
| Bluetooth battery | **BlueZ over D-Bus**, as today | PipeWire doesn't expose it |
| Ducking | a **long-term goal**, not specified here | §10 |

## 3. PipeWire and WirePlumber

- **PipeWire** is the engine. It keeps a graph of *nodes* (outputs,
  microphones, an app's stream, filters), their *ports* and *links*, and moves
  audio buffers in real time. It deliberately makes no decisions.
- **WirePlumber** is the session manager: an ordinary PipeWire client that
  holds the policy. It:
  - creates the ALSA and Bluetooth devices and picks their profiles and
    routes;
  - links each new stream to the right output;
  - computes the default output and input;
  - remembers volumes, profiles and each app's chosen output in
    `~/.local/state/wireplumber`;
  - switches Bluetooth headsets to call mode when an app records.
- **libwireplumber** is the library WirePlumber is built on. Clients like
  `wpctl` use it too. Its `mixer-api` and `default-nodes-api` plugins encode
  the conventions a client must follow:
  - a sound card's volume is written to the device's **Route** parameter
    with `save: true`, so that WirePlumber keeps it and the hardware mixer
    is used; only nodes without a device (virtual outputs, app streams) take
    the node's `Props`;
  - the default output is chosen by writing `default.configured.audio.sink`
    in the `default` metadata, never by linking streams ourselves.

Vela follows the same conventions, so whatever it changes is what WirePlumber
restores at the next login, and what any other tool (`wpctl`, pavucontrol)
sees.

## 4. Overview

```
 vela-shell                             vela-settings
   SystemStatus  Mixer  OSD  tray         Sound  Properties  Mixer
        \          |    |    /               \       |       /
         vela::audio::Audio (QML-facing)      vela::audio::Audio
                     |                                 |
         vela::audio::Core ── libwireplumber ──┐       Core ── ...
                     |       (WpCore, plugins) │
                     └── libpipewire (pw_stream: levels, test sound)
                                               │
                      PipeWire  ◄──────────────┘  ◄── WirePlumber
```

### 4.1 Event loop

- The `WpCore` is created on the default `GMainContext`. On Linux Qt runs on
  GLib's main loop, so WirePlumber's callbacks arrive in the interface thread
  between Qt's events. They update the model and emit Qt signals directly.
- At startup the library checks that the event dispatcher is GLib's
  (`QT_NO_GLIB` or a Qt built without GLib). If not, audio is reported as not
  available, with a message in the log.
- The audio samples never pass through Vela's threads. The only streams Vela
  opens (level meters, test sound, §7) are processed by PipeWire's real-time
  thread inside our process, which libpipewire creates by itself.

### 4.2 Connection

- `Core` connects at startup. If PipeWire isn't running, audio is not
  available and the core retries with a backoff (0.5 s, then up to 5 s).
- When PipeWire goes away (`disconnected`), the model is emptied, the
  interface shows audio as not available, and the core reconnects.
- A WirePlumber restart (§8) doesn't drop the connection: the `default`
  metadata and the device objects disappear and come back, and the object
  managers follow them. Until the metadata is back, defaults are unknown,
  not "none".

## 5. The model

`vela::audio::Core` keeps one model, filled by WirePlumber object managers
(devices, nodes, links, the `default` metadata). It contains only what the
server says: optimistic values exist only in the Qt layer (§5.4).

### 5.1 Devices and endpoints

- **Device** (`WpDevice`, a sound card or a Bluetooth headset): name,
  description, icon, Bluetooth address, its profiles (`EnumProfile`) and the
  active one, its routes (`EnumRoute`, `Route`).
- **Endpoint**: an output (`Audio/Sink`) or input (`Audio/Source`) node. It
  has:
  - `name` (`node.name`, stable across restarts), description, icon;
  - volume, mute, per-channel volumes and channel positions, the device's
    volume step (`step` from `mixer-api`, the Route's `volumeStep`);
  - `available`: false when its route says the jack is unplugged (the
    Route's `available` is `no`). Unplugged endpoints aren't offered, except
    the current default, as today;
  - `isDefault`;
  - the Bluetooth battery, if any (BlueZ, §2).
- Sink monitors are not inputs: they are not microphones.
- Icons: `device.icon-name` when set, otherwise from `device.form-factor`
  and `device.bus` (headset, headphones, speakers, HDMI/DisplayPort,
  USB...).

### 5.2 Streams and apps

- **Stream**: a `Stream/Output/Audio` (playback) or `Stream/Input/Audio`
  (recording) node: app identity, volume, mute, the endpoint it is linked
  to, the endpoint chosen for it (`target.object` in the metadata; none: it
  follows the default), whether it is running.
- **App**: streams grouped by program, one entry per app even with several
  streams (a browser's tabs). Key, as today: `application.process.binary`,
  else `application.name`, else `node.name`.
- **Identity**: the core reports the raw ids (`pipewire.access.portal.app_id`,
  `application.id`, the binary, `application.name`,
  `application.icon-name`). Each process resolves them to a `.desktop` file
  with a resolver it passes in. The shell passes `AppModel`. Settings needs
  the same matching rules (today `AppModel::findDesktopId`). Those rules
  move to `common/` so that both processes name an app the same way.
- Vela's own streams (§7) present themselves as Vela Settings
  (`application.id` = `vela-settings`).

### 5.3 Volume

- **Scale:** cubic, as `wpctl` and pactl show it (`mixer-api`'s `scale`
  property set to cubic). 1.0 = 100%.
- **Grid:** `volumesteps.h` stays as it is: 2 points, or the device's own
  positions when farther apart. The step comes from the model, so the
  blocking read in `deviceIntervals()` goes away.
- **Maximum 100%.** Vela never writes more than 1.0. A value above 100% set
  by another program is shown as 100% (slider at the end) and left alone;
  the first step down lands back on the grid.
- **Overall volume and balance.** The overall volume is the loudest channel,
  and balance is the ratio between channels (as PulseAudio did).
  - Changing the overall volume scales every channel and keeps the balance.
  - Vela therefore always writes per-channel volumes (`channelVolumes`),
    never the single `volume` that would make the channels equal.
- **Mute** is separate from volume, as today.

### 5.4 Writes and the interface

- Every action (setting a volume, mute, default, an app's output or input,
  a profile) is one asynchronous call. No queue of commands is needed: calls
  reach PipeWire in order.
- A slider being dragged: at most one write per event-loop turn, with the
  latest value.
- The Qt layer shows the value just set at once. While its own write is on
  the way, it ignores the server's older values for that object, until the
  value it wrote comes back or 500 ms pass. That replaces today's revisions,
  queues and debounce timers.
- Microphone mute and default input are privacy-related. They are shown as
  pending until the server confirms them, never optimistically, as Settings
  does today.

## 6. Who uses it

### 6.1 Shell

- **`SystemStatus`**: volume, mute and icon of the default output, the volume
  keys and the knob, from the core. The `wpctl` and `pactl subscribe`
  processes go away.
- **Microphone mute key** (`XF86AudioMicMute`): the compositor forwards it
  like the volume keys (`mic mute`, in `bindings.c`). The shell toggles the
  default input's mute and shows the indicator with the microphone icon.
  Shown after the server confirms (§5.4).
- **Volume indicator** (`VolumeOsd.qml`): unchanged, plus the microphone
  variant.
- **Quick settings mixer**: outputs; each app's volume, mute, output and now
  **input** (for apps that record); a level bar on each app (§7.1). It
  always follows the system. Today it follows only while open, but with
  events instead of processes that costs nothing.
- **Wheel on a taskbar button**: one step for that app, as today. No more
  "stale cache" path: the model is always current.
- **Microphone in use** (§7.2): an icon in the tray, with the apps in its
  tooltip.
- `systemactions.cpp` stops offering `kcm_pulseaudio` or pavucontrol for
  "volume-mixer": Settings' Mixer page replaces them.

### 6.2 Settings

Three pages, as in Windows 11.

- **Sound**
  - output: the device to use, its volume and mute;
  - input: the device to use, its volume and mute, and the **input level**
    (the bar that moves when you speak, §7.1);
  - links to every device's properties and to the mixer.
- **Device properties**, one page per output or input:
  - name, which can be changed (§8);
  - profile, including turning the device **off** (the "off" profile). On a
    Bluetooth headset, the profile is also the **codec**: PipeWire offers one
    profile per codec (`a2dp-sink-sbc`, `a2dp-sink-sbc_xq`, `a2dp-sink-ldac`...)
    plus call mode, and WirePlumber remembers the choice. No restart needed;
  - **balance** (left/right, for outputs with two or more channels);
  - **format**: sample rate and sample format (§8);
  - **test** (outputs, §7.3).
- **Mixer**: every app, with volume, mute, output, input and level; the
  outputs and inputs; "reset", which removes every app's chosen output and
  input (they follow the defaults again).

## 7. Vela's own streams

These are made with `pw_stream` from libpipewire, on the core's own
connection (`wp_core_get_pw_core`).

### 7.1 Levels

- A capture stream with `stream.monitor = true` and `resample.peaks = true`
  (the same PipeWire uses for pavucontrol's meters). PipeWire then computes
  the peak at a low rate, so the cost is negligible.
- **Input level**: on the default input, only while Settings' Sound page is
  visible.
- **App levels** (both mixers): one monitor stream per app stream, on that
  stream's node, only while a mixer is visible. They're opened and closed
  with the streams; at most one per stream.
- The bar shows the peak with a short decay, like Windows.

### 7.2 Microphone in use

- An app uses the microphone when one of its recording streams is
  **running** and linked to an `Audio/Source` (a microphone, or a virtual
  source made from one). Recording an output (screen recorders capturing
  desktop audio, sink monitors) doesn't count. Neither do the internal
  streams of filters (the nodes of a `node.link-group`).
- Settings' input level meter counts: while the Sound page is visible, the
  tray says "Settings is using your microphone".
- The tray icon appears while at least one app uses it. The tooltip lists
  the apps, and a click opens Settings > Sound.

### 7.3 Test sound

- A short chime synthesized in code (two soft tones, float PCM), played on
  the chosen output only (`target.object`), with `node.dont-reconnect`. If
  that output goes away, the sound stops instead of moving elsewhere.
- **First on the left channel, then on the right**, so balance and wiring can
  be checked. Mono outputs play it once. Outputs with more channels play it
  on the front left, then the front right.
- A second press while it plays starts it again.

## 8. Settings stored in WirePlumber

Two mechanisms, as decided.

- **Runtime settings** (`WpSettings`, saved by WirePlumber in its own state):
  for what WirePlumber already exposes. Today that means Bluetooth's
  automatic switch to call mode (`bluetooth.autoswitch-to-headset-profile`)
  and, one day, ducking (§10). They apply immediately.
- **Vela's file**, `~/.config/wireplumber/wireplumber.conf.d/90-vela.conf`:
  - **device names** (`node.description` through `monitor.alsa.rules` /
    `monitor.bluez.rules`, matched on `node.name`);
  - **format and sample rate** (`audio.format`, `audio.rate`,
    `audio.allowed-rates` on ALSA nodes).

  Rules for the file:
  - Vela regenerates it whole from its own list, with an atomic write
    (`QSaveFile`). It never reads, edits or creates any other file in that
    directory. A user's own rules keep working; files later in the order
    override ours.
  - After a change, WirePlumber is restarted through systemd's user manager
    (D-Bus `RestartUnit("wireplumber.service")`, no process started). The
    page says before applying that **sound stops for about a second**.
  - Names are matched on `node.name`. If a device stops existing, its rule
    stays (harmless) until Vela rewrites the file.

## 9. Library layout

- `common/audio/` → static library `vela-audio` (Qt Core, `wireplumber-0.5`,
  `libpipewire-0.3`).
  - `Core`: connection, object managers, the model, actions (§4, §5).
  - `Audio`: the QML-facing object both processes register (outputs,
    inputs, apps, default output's volume, microphone in use...), with the
    optimistic layer (§5.4).
  - `LevelMeter`, `TestSound`: §7.
  - `WireplumberConfig`: §8.
- What goes away:
  - in the shell, `mixer.cpp` and the audio part of `systemstatus.cpp`
    (both become thin users of `Audio`);
  - in Settings, `audio.cpp`;
  - `asyncprocess.h` stays as long as networking uses it.
- GObject code stays inside `Core` and `WireplumberConfig`. No GLib types
  appear in the headers the shell and Settings include.

## 10. Not in scope

- **Ducking** (lowering other sounds during a call) is a long-term goal,
  left unspecified on purpose. WirePlumber 0.5 has the pieces (role-based
  linking, `linking.role-based.duck-level`), but it only works for apps that
  declare `media.role` = Communication, and turning role-based linking on
  changes how every stream with a role is linked. To be studied with real
  call apps before deciding anything.
- Excluded:
  - echo cancellation and noise suppression;
  - mono audio;
  - spatial sound and Windows-style "audio enhancements" (no common base on
    Linux);
  - the system sounds volume (Vela has no system sounds).

## 11. Tests

Automated tests run against a **real PipeWire and WirePlumber**, started by
the test in a private `XDG_RUNTIME_DIR`, `XDG_STATE_HOME` and
`XDG_CONFIG_HOME` under the build directory. They never touch the user's
audio or WirePlumber state.

- **Fixture**:
  - PipeWire with a minimal configuration and fake devices
    (`support.null-audio-sink`): a stereo output, a second output, a 5.1
    output, a microphone;
  - WirePlumber with the ALSA, Bluetooth and camera monitors disabled;
  - a small test client (`tests/audio/fake-stream.c`) that opens playback or
    recording streams with chosen properties (binary, app name, portal app
    id, media role) and a known amplitude.
- **Library (`audio.*`)**:
  - outputs, inputs, defaults, apps grouped by program;
  - changes from outside (`wpctl` in the test) arriving as events;
  - volume on the grid, steps, the 100% cap, a value above 100% set from
    outside;
  - balance kept while stepping;
  - mute, with the microphone shown as pending until confirmed;
  - an app's output and input: set, cleared, restored by WirePlumber when
    the stream comes back;
  - outputs appearing and disappearing;
  - PipeWire restarted (reconnection) and WirePlumber restarted (defaults
    unknown, then back);
  - levels within a tolerance of the known amplitude;
  - "microphone in use": a recording stream on the microphone counts, one on
    an output's monitor doesn't, Settings' meter does;
  - the test sound reaching only the chosen output, left channel then right
    (checked by recording that output's monitor);
  - `90-vela.conf` written atomically, other files in the directory left
    untouched, a rename visible after the restart.
- **Interface (functional)**: the volume indicator (keys, microphone mute
  key), quick settings mixer (outputs, app volume/output/input), the wheel
  on a taskbar button, the tray's microphone icon, Settings' three pages.
  All against the same fixture, with `vela-shot` comparisons where useful.
- **Bluetooth battery**: a fake BlueZ on a private system bus, like
  `fake_polkitd.py`.
- **CI**: the Arch images install `pipewire` and `wireplumber`.
- **By hand, by the user, on real hardware** (added to
  [hardware](hardware.md)):
  - volume keys and knob on a real sound card (hardware mixer through the
    Route, the volume remembered after logging out);
  - unplugging and plugging headphones (unplugged hidden, default
    switching);
  - a sound card's profiles, HDMI/DisplayPort audio, turning a device off;
  - a Bluetooth headset: battery, codecs, call mode, devices with few volume
    positions;
  - the microphone mute key and, on laptops, its LED;
  - format and sample rate on a USB DAC;
  - a rename across a WirePlumber restart.

## 12. Packaging and documentation

- PKGBUILD:
  - `pipewire` and `wireplumber` become dependencies (libraries are linked);
  - `libpulse` and the "volume" optional dependencies go;
  - `pipewire-pulse` stays recommended for apps that still speak
    PulseAudio, but Vela doesn't use it.
- [hardware](hardware.md): "Audio controls and per-app volumes" becomes
  "PipeWire and WirePlumber"; the manual tests of §11 are added.
- README: PulseAudio without PipeWire is not supported.

## 13. Stages

| Stage | Content | |
|---|---|---|
| A0 | Test fixture (§11), CI packages | |
| A1 | `vela-audio`: connection, model, volume (grid, cap, balance), defaults, apps and their outputs/inputs, reconnection; library tests | |
| A2 | Shell on the library: system volume, OSD, microphone mute key (compositor + OSD), quick settings mixer with inputs, taskbar wheel; `wpctl`/`pactl`/`pw-*` gone from the shell | |
| A3 | Settings on the library: Sound page with input level, device properties (profile/off, codec, balance, test sound), Mixer page with levels; `audio.cpp` gone | |
| A4 | Microphone in use in the tray | |
| A5 | `90-vela.conf`: names, format and rate, with the WirePlumber restart | |
| A6 | PKGBUILD and docs (§12); the user's tests on real hardware | |

## 14. Open questions and risks

- **Format and graph clock.** PipeWire runs the whole graph at one clock
  rate (`default.clock.rate`, switched among `default.clock.allowed-rates`).
  Forcing a device's `audio.rate` only decides what that device is opened
  at; the graph may still resample. What "format" should mean in Vela has to
  be checked on a USB DAC before A5. Choices: the device's rate only, or
  also allowing the graph to follow it.
- **Hardware volume through the Route** (§3) is left to `mixer-api`. It is
  only covered by the manual tests, since null sinks have no routes.
- **Bluetooth codec profiles**: which codecs appear depends on the headset
  and on PipeWire's build. The page shows what the device offers and makes
  no promises.
- **"Microphone in use" edge cases**: virtual sources (echo cancellers,
  noise filters installed by the user) and apps that keep a paused recording
  stream. The rule in §7.2 (running + linked to a source) is to be checked
  with real apps (browsers, Discord, OBS).
- **Rename rules for null sinks**: the tests need `90-vela.conf`'s rules to
  match the fixture's fake devices too. To check in A0, otherwise the rename
  is only tested by hand.
- **libwireplumber API changes**: 0.4 → 0.5 broke clients. Vela pins
  `wireplumber-0.5` at build time; a 0.6 would mean a port of `Core`.
