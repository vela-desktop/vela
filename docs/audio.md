# Audio

Design of Vela's audio backend: one native library, shared by the shell and
Settings. It replaces today's three backends, which drive command-line tools
(`wpctl`, `pactl`, `pw-dump`, `pw-metadata`). This document comes before the
code: decisions are made here, and the code follows them.

> **Status:** design approved. **A0 to A5 are done** (§13): the test
> fixture, the `vela-audio` library, the shell and Settings on it, the
> microphone in use in the tray, Vela's files (names, the automatic sample
> rate, each device's rates and format). A6: packaging and docs are done;
> the tests on real hardware ([hardware](hardware.md#sound)) are next.

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
| Vela's settings | WirePlumber's **runtime settings** where they exist; otherwise **two files owned by Vela**: `~/.config/wireplumber/wireplumber.conf.d/90-vela.conf` (per device) and `~/.config/pipewire/pipewire.conf.d/90-vela.conf` (the graph's sample rates) | changes to WirePlumber's file need a WirePlumber restart, with a short audio gap, said before applying; the sample rates apply at once (§8) |
| Sample rate | **automatic by default**: the mix follows what plays (44.1 kHz music is not converted), on every device | better than Windows, whose shared mode converts everything to one fixed format. When a device has trouble, the fix is a setting on that device, even at the cost of an audio restart, never lower quality for everyone (§8.1) |
| "Microphone in use" | **honest**: Settings' own level meter counts too | an indicator that hides someone, even us, can no longer be trusted (§7.2) |
| Test sound | **synthesized** in code, left channel then right | no sound file to ship, no decoder (§7.3) |
| Bluetooth battery | **vela-bluetooth** ([Bluetooth](bluetooth.md) §8) | PipeWire doesn't expose it; one Bluetooth client |
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
  - the Bluetooth battery, if any (vela-bluetooth, §2).
- Sink monitors are not inputs: they are not microphones.
- A **suspended** node may report fewer volumes than it has channels: two,
  until its format is set (seen in A1 on the fixture's 5.1 output). The
  layout comes from `audio.position` / the channel map; the volumes are
  what the node reports.
- A device also has its profiles (`EnumProfile`) and the active one
  (`Profile`), each with its availability: a profile whose ports have
  nothing plugged in (HDMI without a display) isn't offered, except the
  active one. A device whose active profile is `off` is **turned off**: its
  outputs and inputs are gone, the device stays. Done in A3.
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
  with a resolver it passes in. Both pass the same one,
  `audioAppResolver` (`shell/src/audioapps.h`), on `AppModel`, whose source
  Settings compiles too: both processes name an app the same way.
- Vela's own streams (§7) present themselves by the program's name
  (`application.id` = `vela-settings`, `vela-shell`): Settings' test sound
  is named after Settings' `.desktop` file.

### 5.3 Volume

- **Scale:** cubic, as `wpctl` and pactl show it (`mixer-api`'s `scale`
  property set to cubic). 1.0 = 100%.
- **Grid:** `volumesteps.h` stays as it is: 2 points, or the device's own
  positions when farther apart. The step comes from the model, so the
  blocking read in `deviceIntervals()` goes away.
  - Readback within 0.6 percentage points of a grid position counts as on
    that position (the same tolerance as the optimistic-write confirmation).
    A sound card's hardware mixer may report e.g. 59.6% for a requested 60%,
    or 96.0007% for 96% on a FiiO K11 in 1 dB steps. The next knob/wheel
    event must move to 62% / 98%, not request 60% / 96% again. Values farther
    off-grid still move to the next grid position in the chosen direction.
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

- **`Audio`** (the library's object) is registered for QML as `Audio`; the
  shell names apps through its `AppModel` (portal app id, `application.id`,
  binary, app name). `SystemStatus` has no audio any more: the `wpctl` and
  `pactl subscribe` processes are gone, also from Settings, which creates a
  `SystemStatus` too. Done in A2.
- **`SoundKeys`** (`shell/src/soundkeys.cpp`, registered as `Sound`): the
  volume keys and the knob, the wheel on the taskbar's volume icon and on an
  app's button, each asking for the indicator. Done in A2.
- **Microphone mute key** (`XF86AudioMicMute`): the compositor forwards it
  like the volume keys (`volume mic-mute`, in `bindings.c`). The shell
  toggles the default input's mute; the indicator, with the microphone's
  icon and "Microphone on/off", appears once the server confirms, or after
  2 s showing the unchanged state. Done in A2.
- **Volume indicator** (`VolumeOsd.qml`): unchanged, plus the microphone
  variant (no slider). Done in A2.
- **Quick settings mixer**: outputs, with a Bluetooth headset's battery
  (from the process's vela-bluetooth client, matched by the headset's
  address);
  each app's volume, mute, output and now **input** (for apps that record).
  It always follows the system. Done in A2; the level bars in A3 (below).
- **Wheel on a taskbar button**: one step for that app, as today. No more
  "stale cache" path: the model is always current. Done in A2.
- **Microphone in use** (§7.2): an icon in the tray, with the apps in its
  tooltip. Done in A4.
- **Sample rate**: at start the shell makes "Automatic" the default where
  nobody chose (§8.1). Done in A5.
- **Level bars**: each app's slider in the quick settings mixer has a
  thin bar under it (§7.1), only while the sound page is open. Done in A3.
- "volume-mixer" opens Settings' Volume mixer page, and the quick settings'
  "More volume settings" too, like Windows; `systemactions.cpp` no longer
  offers pavucontrol or `kcm_pulseaudio` for it. Done in A3.

### 6.2 Settings

Three pages, as in Windows 11, on the library since A3 (`audio.cpp` and
`pactl` are gone from Settings).

- **Sound** (`SoundPage.qml`)
  - output: the device to use, its volume and mute;
  - input: the device to use, its volume and mute, and the **input level**
    (the bar that moves when you speak, §7.1);
  - an arrow on every device leading to its properties; under "Advanced",
    the devices turned off (to turn them back on) and the mixer.
- **Device properties** (`SoundDevicePage.qml`, page `sound-device`), one
  page per output or input, the device's name in the path:
  - set as default;
  - "Use this device": turning the device **off** (its "off" profile) and
    on again (its best available profile, by priority);
  - profile. On a Bluetooth headset, the profile is also the **codec**:
    PipeWire offers one profile per codec (`a2dp-sink-sbc`,
    `a2dp-sink-sbc_xq`, `a2dp-sink-ldac`...) plus call mode, and
    WirePlumber remembers the choice (§3). No restart needed;
  - volume and mute;
  - **balance** (left/right, for endpoints with channels on both sides),
    like PulseAudio's: the louder side keeps its level, center and LFE are
    left alone, the volume stays. It can't be set in silence;
  - **test** (outputs, §7.3), or the **input level** (inputs);
  - the **name**, which can be changed (§8, A5);
  - **format** (§8.1, A5): what the device runs at right now ("44100 Hz,
    24 bit") and which apps are converted to it, live; its sample rates
    ("Follow the system", or always one rate) and its sample format
    ("Best", or one of those it offers, shown only when there are two or
    more depths). Not on Bluetooth devices, where the codec decides (the
    profile, above). The name can be changed for ALSA and Bluetooth
    devices, the only ones WirePlumber's monitors make. Done in A5.
- **Sound › Advanced**, from A5: the **sample rate** ("Automatic", or fixed
  at 48000 Hz like Windows, §8.1) and **restart audio** (WirePlumber and
  PipeWire, for when something is stuck). Done in A5.
- **Mixer** (`VolumeMixerPage.qml`, page `volume-mixer`): the default output
  and input with their volume; every app with volume, mute, level, output
  and input; "Reset", which removes every app's chosen output and input
  (they follow the defaults again).

## 7. Vela's own streams

These are made with `pw_stream` from libpipewire, on the core's own
connection (`wp_core_get_pw_core`), and destroyed before it closes
(`Core::aboutToDisconnect`). All of them say `state.restore-props` and
`state.restore-target` = false, so WirePlumber neither remembers nor
restores anything for them, and `node.dont-reconnect`, `node.dont-fallback`
and `node.dont-move`, so they never end up somewhere else. Done in A3.

### 7.1 Levels

- `LevelMeter` (`levelmeter.h`), a QML type (`Vela.Audio`): a capture
  stream with `stream.monitor = true` and `resample.peaks = true` (the same
  PipeWire uses for pavucontrol's meters), F32 at 100 Hz. PipeWire then
  computes the peak at a low rate, so the cost is negligible. Samples are
  read on PipeWire's real-time thread; the interface reads the peak every
  30 ms.
- An output's meter records its monitor (`stream.capture.sink`). Meters on
  outputs and apps are passive (`node.passive`): they don't keep anything
  running. An input's meter is not: it opens the microphone, and that
  counts (§7.2).
- **Input level**: on the default input, only while Settings' Sound page is
  visible.
- **App levels** (both mixers): one monitor stream per app stream, on that
  stream's node, only while a mixer is visible. They're opened and closed
  with the streams; at most one per stream.
- The bar shows the peak on the volume's scale (cubic), with a short decay,
  like Windows: from the top to nothing in under a second.

### 7.2 Microphone in use

- An app uses the microphone when one of its recording streams is
  **running** and actually linked to an `Audio/Source` (a microphone, or a virtual
  source made from one). Recording an output (screen recorders capturing
  desktop audio, sink monitors) doesn't count. Meters on other streams
  don't either: they record an app, not an input. A filter stream linked
  to an input **does** count: `node.link-group`, `node.virtual`,
  `stream.monitor` and `stream.capture.sink` are client-supplied and cannot
  be used to hide an active input link. Vela prefers a conservative
  visible indicator over false negatives for privacy.
- The indicator follows observed PipeWire links; it is not a permission
  boundary or a guarantee about hardware recording outside PipeWire.
- Settings' input level meter counts: while the Sound page is visible, the
  tray says "Settings is using your microphone" (its stream is a monitor,
  but of an input).
- `Audio::microphoneUsers`: the apps, named like in the mixers (§5.2), in
  the order they started. One that stops (closed or paused) stays **about
  a second**, so that a pause or a page change doesn't make the icon blink;
  one that starts shows at once, so the icon never hides a real use.
- The tray icon (`Taskbar.qml`, left of the system icons) appears while at
  least one app uses it. Its tooltip, after half a second, lists the apps
  ("Vela Player is using your microphone"), and a click opens Settings >
  Sound. The tooltip is a small panel of its own, `TaskbarTooltip.qml`
  (`Menus.tooltip`), placed above the taskbar like the window previews; the
  mouse goes through it.
- Done in A4.

### 7.3 Test sound

- `TestSound` (`testsound.h`): a short chime synthesized in code (two soft
  tones, 880 and 659 Hz, float PCM), played on the chosen output only
  (`target.object`). If that output goes away, the sound stops instead of
  moving elsewhere.
- **First on the left channel, then on the right**, so balance and wiring can
  be checked: a stereo stream, FL then FR, with `channelmix.upmix = false`
  so that nothing is made up for the other channels. Mono outputs (no left
  and right) play it once. Outputs with more channels play it on the front
  left, then the front right.
- A second press while it plays starts it again; the button becomes
  "Stop" meanwhile.
- A quarter of a second of silence comes first: an output waking up (a USB
  DAC unmuting, a device switching rate) loses its first moments, and cut
  the chime's start on real hardware (A6).

## 8. Settings stored in WirePlumber and PipeWire

Three mechanisms. Vela keeps no copy of what they hold: the files are its
own state, read back to show and to change them.

- **Runtime settings** (`WpSettings`, saved by WirePlumber in its own state):
  for what WirePlumber already exposes. Today that means Bluetooth's
  automatic switch to call mode (`bluetooth.autoswitch-to-headset-profile`)
  and, one day, ducking (§10). They apply immediately.
- **WirePlumber's file**,
  `~/.config/wireplumber/wireplumber.conf.d/90-vela.conf`, one rule per
  device, matched on `node.name` (`monitor.alsa.rules`,
  `monitor.bluez.rules`):
  - the **name** (`node.description`), with the device's own
    (`vela.audio.description`);
  - the **sample rates** (`audio.allowed-rates`) and **sample format**
    (`audio.format`) of an ALSA node (§8.1);
  - what the device offered before Vela restricted it
    (`vela.audio.rates`, `vela.audio.formats`, §8.1).

  After a change of rates or format WirePlumber is restarted (below). The
  page says before applying that **sound stops for about a second**.
  A new **name restarts nothing** (decided in A6): Vela shows the rule's
  name at once, in the shell too (it watches the file); other programs see
  it once WirePlumber starts again, at the next login or "Restart audio".
  Until then the node keeps the name it was made with, so the rule also
  keeps the device's own name: going back to it while the node still has
  the old one leaves a rule giving the own name, which WirePlumber applies
  next time.
- **PipeWire's file**, `~/.config/pipewire/pipewire.conf.d/90-vela.conf`:
  the graph's **allowed rates** (`default.clock.allowed-rates`, §8.1), and
  its default rate (`default.clock.rate`) when it is fixed at another one. It is
  read when PipeWire starts; the same value is set at once in the `settings`
  metadata (`clock.allowed-rates`), which PipeWire applies without a
  restart (seen in A5 on the fixture).

Rules for both files:
- Vela regenerates its file whole, with an atomic write (`QSaveFile`). It
  never reads, edits or creates any other file in those directories. A
  user's own rules keep working; files later in the order override ours.
- Restarts go through systemd's user manager (D-Bus `RestartUnit`, no
  process started): `wireplumber.service` after WirePlumber's file changes;
  `pipewire.service` for "Restart audio" (WirePlumber and `pipewire-pulse`
  are bound to it and follow). Only "Restart audio" restarts PipeWire;
  the system sample rate restarts nothing.
- Why rates and formats restart WirePlumber: it reads its rules only when
  it starts, and makes the node with them. An ALSA node
  does take `audio.allowed-rates` and `audio.format` at run time (its
  `Props` `params`), but on real hardware (PipeWire 1.6.9, a USB DAC) the
  node failed to start after the change ("Start error: Operation not
  supported") and WirePlumber had to make it again. Tried in A6; not
  used.
- WirePlumber writes its state (defaults, volumes, profiles, each app's
  choices) about **one second after a change**, and does not write it when
  it exits. Seen in A0, and covered by a test. A restart sooner than that
  loses the last changes. Vela therefore restarts only once 1.5 s have
  passed since its own last change.
- Names are matched on `node.name`. If a device stops existing, its rule
  stays (harmless) until Vela rewrites the file.

### 8.1 Sample rate and format

How PipeWire works (PipeWire 1.6, its sources and the fixture):
- The whole graph of a device mixes at **one rate**. When the device is
  idle and a stream starts, the graph takes the highest rate its streams
  ask for, if that rate is in the **allowed rates**; otherwise the default
  (48000). While the device plays, the rate doesn't change: a stream
  starting then is converted.
- The allowed rates are `[ 48000 ]` by default, so today everything is
  converted to 48 kHz. Measured on the fixture: with `[ 44100 48000 96000 ]`
  set in the `settings` metadata, 44.1 kHz and 96 kHz files ran the graph at
  their own rate, without a restart.
- An ALSA device opens at the graph's rate when it can. Its
  `audio.allowed-rates` restricts that: at any other rate the device is
  opened at the nearest allowed one and converted. Its `audio.format`
  restricts the sample format (the default is the best one it has).
- A device lists (`EnumFormat`) its rate range and formats, but only those
  allowed once restricted: Vela keeps what it saw before restricting in
  the rule itself (`vela.audio.rates`, `vela.audio.formats`), to offer the
  other choices later.
- Upstream doesn't allow more rates by default "because of various kernel
  and Bluetooth issues". Vela does: when a device has trouble switching
  (an HDMI receiver that mutes for a second at every switch), that device
  gets a fixed rate. The others keep the quality.

What Vela does:
- **System sample rate** (Sound › Advanced): "Automatic" (the default)
  allows `44100 48000 88200 96000 176400 192000`; "N Hz", one of those,
  allows only that one, so everything is converted to it (like Windows'
  default format; 48000 Hz is PipeWire's default). With one allowed rate
  PipeWire always picks it, but it drops an allowed list that doesn't hold
  its default rate (`clock.rate`) and keeps `[ 48000 ]`: so the default
  rate is set first, to that rate, in the metadata and in the file
  (`default.clock.rate`). Found in A6: the fixture's graph stayed at
  48 kHz with `[ 96000 ]` alone.
  - In "Automatic" the graph often runs at 48 kHz anyway: most of what
    plays (browsers, games, calls) is 48 kHz. Higher rates appear with
    music files at those rates.
  - The default is applied by the shell at start when PipeWire's file is
    missing and the allowed rates are still PipeWire's default (`[ 48000 ]`).
    If they are anything else, someone configured them: Vela leaves them
    and the page shows them as they are, until the user chooses.
- **Device sample rates** (properties): "Follow the system" (no rule) or
  "Always N Hz", N among the standard rates in the device's range.
- **Device sample format**: "Best" (no rule) or one of the device's formats
  ("16 bit", "24 bit", "32 bit").
- **Now** (properties): the device's negotiated format while it runs, and
  each app whose stream runs at another rate ("converted from 44100 Hz").
  Strict bit-perfect playback also needs the volume at 100% or done in
  hardware: the page doesn't promise it.
- Bluetooth devices have no format choices: the codec decides (§6.2).

## 9. Library layout

- `common/audio/` → static library `vela-audio` (Qt Core, `wireplumber-0.5`,
  `libpipewire-0.3`).
  - `model.h`: the snapshot's plain structs (`Endpoint`, `Stream`,
    `AppIdentity`) and the balance math (`scaledChannels`, `balanceOf`).
  - `core.h`, `core.cpp`: `Core`, the connection, object managers, the
    model and actions (§4, §5). A new `WpCore` for every connection; the
    model is rebuilt at most once per event-loop turn, whatever the number
    of events.
  - `audio.h`, `audio.cpp`: `Audio`, the QML-facing object both processes
    register (outputs, inputs, apps, the default output's volume...), with
    the optimistic layer (§5.4). Apps are named through a resolver each
    process passes in (§5.2).
  - `volumesteps.h`: the grid, moved from the shell.
  - `levelmeter.h`, `levelmeter.cpp`, `testsound.h`, `testsound.cpp`: Vela's
    own streams (§7), on libpipewire.
  - The Bluetooth battery is in `Audio` (A2), read from vela-bluetooth since
    Bluetooth's B4.
  - `config.h`, `config.cpp`: `Config`, Vela's two files, the `settings`
    metadata and the restarts through systemd (§8, A5).
- `shell/src/audioapps.h`: the resolver both processes pass (§5.2).
- What went away: in the shell, `mixer.cpp` and the audio part of
  `systemstatus.cpp` (A2); in Settings, `audio.cpp` (A3).
  `asyncprocess.h` stays as long as networking uses it.
- GObject code stays inside `Core` and `Config`. No GLib types
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

- **Fixture** (`tests/audio/`, done in A0):
  - `pipewire.conf`: PipeWire with no hardware, no realtime and fake
    devices (`support.null-audio-sink`): `vela-test-speakers` (stereo, the
    default), `vela-test-headphones` (stereo), `vela-test-surround` (5.1),
    `vela-test-microphone` (mono, `Audio/Source/Virtual`, the default
    input);
  - `wireplumber-test.conf`: WirePlumber's normal policy, defaults and
    state (`-p vela-test`), without the ALSA, Bluetooth and camera monitors
    and without D-Bus;
  - `fixture.py`: starts both in a temporary directory (runtime, state,
    configuration, home) and gives tests an environment that cannot reach
    the user's audio, D-Bus or WirePlumber state. The system bus points at
    a socket nobody listens on (no rtkit, BlueZ or logind). It also runs a
    single command inside: `fixture.py -- wpctl status`;
  - `fake-stream.c`, `vela-audio-fake-stream`: a fake app that plays a sine
    of known amplitude or records and reports per-channel peaks. It takes
    any stream property (binary, app name, portal app id, media role,
    target) and speaks lines of text (`ready`, `state`, `peaks`; `pause`,
    `resume`, `quit`). A non-zero exit (a crash, a sanitizer) fails the
    test;
  - `fake-device.c`, `vela-audio-fake-device` (A3): a fake sound card, a
    device this client exports with profiles (off, stereo, HDMI with
    nothing plugged in, an LDAC codec). It reports every profile set on it
    and whether it is to be saved (`ready`, `profile`; `available`). The
    null sinks have no device, hence no profiles;
  - `test_fixture.py` (`audio-fixture`): devices and defaults, channel
    layouts, playback following the default output, recording from the
    microphone, levels following amplitude and volume, pause and resume,
    WirePlumber and PipeWire restarts, isolation, clean shutdown. About 4 s.
- **Library (`audio-core`, `audio-view`, done in A1)**: `tests/audio/core.cpp`
  and `view.cpp`, QtTest programs run inside the fixture, which they drive
  through its control socket (stop, start and restart the daemons, devices
  appearing and disappearing). About 13 s together. Covered:
  - outputs, inputs, defaults and channel layouts; apps grouped by program,
    named through the resolver; level meters and recordings of outputs not
    counted as apps;
  - changes from outside (`wpctl`) arriving as events;
  - volume on the grid, steps, the 100% cap, a value above 100% set from
    outside shown as 100% and stepped back onto the grid;
  - balance kept while changing the volume, also through silence;
  - a value shown at once, a dragged slider writing only its last value,
    the server winning once the 500 ms window has passed;
  - a step unmuting the output; microphone mute and default input shown
    only once confirmed, a default without WirePlumber reported as an
    error;
  - an app's volume, mute, output and input: set, cleared, reset;
    WirePlumber sending an app to its chosen output again the next time,
    and forgetting it once set back to the default;
  - outputs and inputs appearing and disappearing;
  - PipeWire stopped (not available, actions refused) and back
    (reconnection); WirePlumber stopped (defaults unknown, not "none") and
    back.
- **Library, A3**: in `audio-view`, the balance math; balance set and kept
  across volume changes, also both at once and through silence; devices and
  their profiles (unavailable ones left out, then offered once available),
  an output naming its device; a profile shown at once, set with `save`,
  turning off and on, remembered by WirePlumber across its restart, an
  unknown one reported as an error. `audio-streams` (`streams.cpp`, about
  8 s):
  - output, input and app levels within a tolerance of the known
    amplitude and following the volume; an app's level over all its
    streams, a stream's monitor going with it; meters only while active,
    not counted as apps, back after PipeWire restarts;
  - the chime's shape; the test sound reaching only the chosen output, left
    channel then right (recording that output's monitor, and another one
    staying silent), again from the start, once on a mono output, stopping
    when its output goes away without moving to the default.
  - WirePlumber never links a playing stream to a source, so the fake
    microphone hears a fake voice through a link the test makes (`pw-link`).
- **Library, A4** (`audio-view`): "microphone in use". A recording app
  counts, named through the resolver; paused or closed, it stays about a
  second, then goes, and comes back when resumed. A screen recorder taking
  an output, a filter's own stream (`node.link-group`), meters on an output
  and on an app don't count; an input's meter does; recording a virtual
  source (like an echo canceller's) does.
- **Library, A5**:
  - both `90-vela.conf` written atomically, other files in the directories
    left untouched, read back; WirePlumber accepting its file across a
    restart, and not restarted before 1.5 s from the last change. The
    rename itself is checked by hand (§14);
  - the allowed rates set at once in the `settings` metadata; the default
    applied only when PipeWire's file is missing and the rates are
    PipeWire's own, left alone otherwise;
  - the live format: a 44.1 kHz file playing unconverted with "Automatic",
    converted with "48000 Hz".
- **Library, A6**: a fixed rate other than 48000 (the graph at 96 kHz with
  a 44.1 kHz file, `default.clock.rate` in the file); a rename restarting
  nothing, shown at once and in a second process through the watched
  file, and back to the own name while the node still has the old one;
  steps from hardware-mixer volumes read back slightly off the grid.
- **Shell (`audio-keys`, done in A2)**: `SoundKeys` on the library: volume
  keys with their indicator, a step unmuting, the microphone key whose
  indicator waits for the server, the wheel on an app's taskbar button.
  `audio-view` also covers the Bluetooth battery, against a fake
  vela-bluetooth on a private bus. The compositor forwarding the microphone key is in the
  functional tests.
- **Interface (functional)**, not written yet: the volume indicator (keys,
  microphone mute key), quick settings mixer (outputs, app
  volume/output/input, level bars), the wheel on a taskbar button, the
  tray's microphone icon, Settings' three pages. All against the same
  fixture, with `vela-screen-dump` comparisons where useful. The pages of
  A2 and A3 were checked by hand in a headless session inside the fixture.
- **Bluetooth battery**: a fake BlueZ on a private system bus, like
  `fake_polkitd.py`.
- **CI**: the Arch images install `pipewire` and `wireplumber`.
- **By hand, by the user, on real hardware** (in
  [hardware](hardware.md#sound), A6):
  - volume keys and knob on a real sound card (hardware mixer through the
    Route, the volume remembered after logging out);
  - unplugging and plugging headphones (unplugged hidden, default
    switching);
  - a sound card's profiles, HDMI/DisplayPort audio, turning a device off;
  - a Bluetooth headset: battery, codecs, call mode, devices with few volume
    positions;
  - the microphone mute key and, on laptops, its LED;
  - format and sample rate on a USB DAC: "Automatic" following the
    music, a fixed device rate, a sample format, what the page shows
    live; an HDMI receiver and a Bluetooth headset with "Automatic";
  - "Restart audio";
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
| A0 | Test fixture (§11), CI packages | done |
| A1 | `vela-audio`: connection, model, volume (grid, cap, balance), defaults, apps and their outputs/inputs, reconnection; library tests | done |
| A2 | Shell on the library: system volume, OSD, microphone mute key (compositor + OSD), quick settings mixer with inputs, taskbar wheel; `wpctl`/`pactl`/`pw-*` gone from the shell | done |
| A3 | Settings on the library: Sound page with input level, device properties (profile/off, codec, balance, test sound), Mixer page with levels; `audio.cpp` gone | done |
| A4 | Microphone in use in the tray | done |
| A5 | Vela's files (§8): names, automatic sample rate, device rates and format, live format, restarts | done |
| A6 | PKGBUILD and docs (§12); the user's tests on real hardware | PKGBUILD and docs done |

## 14. Open questions and risks

- **Format and graph clock** (answered in A5, §8.1): the graph follows
  what plays when more rates are allowed, so Vela allows them by default
  and fixes a rate only on the devices that need it. Still to check on real
  hardware: Bluetooth and HDMI with the rates switching (§11).
- **Hardware volume through the Route** (§3) is left to `mixer-api`. It is
  only covered by the manual tests, since null sinks have no routes.
- **Bluetooth codec profiles**: which codecs appear depends on the headset
  and on PipeWire's build. The page shows what the device offers and makes
  no promises.
- **"Microphone in use" edge cases**: virtual sources (echo cancellers,
  noise filters installed by the user) and apps that keep a paused recording
  stream. The rule in §7.2 (running + linked to a source) is to be checked
  with real apps (browsers, Discord, OBS).
- **Rename in the tests** (answered in A0): `monitor.alsa.rules` and
  `monitor.bluez.rules` apply to the nodes WirePlumber's monitors create.
  The fixture's null sinks are made by PipeWire itself, so no rule renames
  them. The automated tests check the file and that WirePlumber accepts it;
  the rename itself is in the manual tests.
- **libwireplumber API changes**: 0.4 → 0.5 broke clients. Vela pins
  `wireplumber-0.5` at build time; a 0.6 would mean a port of `Core`.

## 15. Investigating intermittent Fedora audio test failures

The Fedora workflow (`.github/workflows/fedora.yml`) includes **opt-in,
non-blocking** audio diagnostics. Normal PR and main builds retain the existing
required CTest gate; they do **not** run the extra stress attempts.

To investigate a failure:

1. Open **Actions → Fedora Build → Run workflow**.
2. Select the branch containing the diagnostic workflow and set
   `audio_diagnostics` to `true`.
3. Inspect the **Compare isolated and full audio-core runs** step and download
   the `fedora-audio-diagnostics` artifact (retained for seven days).

This runs three fresh private PipeWire/WirePlumber fixtures for each mode:
`isolated` (the `chosenOutputIsRemembered` test), `isolated-gdb` (the same
test under GDB), and `full-suite` (the complete audio-core Qt test binary).
The artifact contains one log per attempt and `summary.csv` with the
classification (`pass`, `crash`, `qt-assertion`, `timeout`, or
`other-error`).

GDB may be unavailable due to ptrace restrictions; a non-reproduction is not
evidence that a timing race is fixed. The optional diagnostic step does not
replace or relax the original required Fedora CTest checks.
