# Screen sharing

Design of what Vela shows while the screen, or a window, is being shared or
recorded: an icon in the taskbar next to the microphone's, a small panel
that says who is capturing what, and notifications held back meanwhile. This
document comes before the code: decisions are made here, and the code
follows them.

> **Status:** design proposed; S1 and S2 done (§10), S3 to S5 in progress
> on the same branch.

## 1. Goals

1. **Honest.** The icon is there whenever an app outside the shell is
   capturing the screen or a window, whatever way it captures: the portal
   (browsers, Discord, OBS) or the Wayland protocols directly (wf-recorder,
   OBS's wlrobs). Like the microphone's (audio.md §7.2), it prefers a
   visible icon to a missed capture, and it cannot be turned off.
2. **Precise.** Screenshots don't light it, and nothing is guessed from
   timers that poll: the compositor sees every capture session and every
   frame, because the sources are its own.
3. **Says who and what.** "Firefox is sharing your screen", "OBS Studio is
   sharing a window of Konsole": the app by the name Start gives it, the
   output or window by the name the user knows.
4. **Private notifications stay private.** While something is shared,
   notification popups are held back in the notification center, unless
   the user turned that off. The user's own Do not disturb is never changed.
5. **Tested without a browser.** Functional tests capture with real
   protocol clients; the PipeWire part runs on a private PipeWire.

## 2. Decisions taken

| Decision | Choice | Consequence |
|---|---|---|
| Who decides that the screen is captured | **the compositor**, from its own capture sources and the screencopy frames (§3) | no heuristics in the shell, no polling of client objects |
| What counts | an **ext-image-copy-capture session open for 1 s**, or **wlr-screencopy frames of the same output within 2 s** of each other, from any client but the shell (§3) | a screenshot (one short session, one frame per output) never counts |
| The shell's own captures | **not reported**: Alt+Tab, taskbar previews, the source chooser | the client is recognized by its pid (§4.2) |
| How the shell hears it | **`watch capture`** on the command socket, the whole state as one JSON line after every change (§4.1) | a restarted shell gets the state when it reconnects; no "resend now and then" |
| The app behind the portal | **PipeWire**: the apps whose video streams are linked to the portal's nodes (§5.2) | the browser, not "xdg-desktop-portal-wlr"; when unknown, "An app" |
| The icon | always shown while captured, on the primary taskbar, left of the microphone; **no switch to hide it** | an indicator that can be hidden can't be trusted (as the microphone's) |
| Click | a **small panel** listing each app and what it captures (§6) | there is no "stop" button: a capture is the app's, the compositor can't end it without killing the client |
| Notifications | **an automatic Do not disturb while sharing, on by default**, a switch in Settings > System > Notifications (§7) | added to the user's own Do not disturb, never written into it |
| Recordings of Vela itself | the compositor's **`VELA_CAPTURE_UNREPORTED=1`** (§4.3) | the trailer and journal videos don't show the icon; only whoever starts the compositor can set it |

## 3. What counts as capturing

The facts this rests on, checked in the sources of wlroots 0.20.2 and
xdg-desktop-portal-wlr 0.8.4 (Vela's portal for screen sharing,
`session/vela-portals.conf`):

- **xdg-desktop-portal-wlr prefers ext-image-copy-capture** when the
  compositor offers it, as Vela does, both for outputs and for windows. It
  opens **one session for the whole share** and closes it when the share
  ends (`xdpw_ext_ic_session_close`), even while the PipeWire stream is
  paused. wlr-screencopy is only its fallback.
- **The capture sources are Vela's**: `scene/output_capture.c` (one source
  per client and output) and `scene/capture.c` (one per window). wlroots
  announces each new session (`wlr_ext_image_copy_capture_manager_v1`'s
  `new_session`) with its client and its source, and its `destroy`.
- **wlr-screencopy** has no events at all, but each frame is a protocol
  object created by a request (`capture_output`): the compositor sees it
  being created with `wl_client_add_resource_created_listener`, and it
  carries its output.
- A **screenshot** (grim, Spectacle through the portal, which runs grim, the
  shell's Snipping Tool) opens a session or a frame per output, waits for
  one frame and closes it: a few tens of milliseconds.

The rules, per client:

- An **ext-image-copy-capture session counts once it has been open for
  1 s**, and as long as it stays open, frames or not: the client can take a
  frame at any moment. Its target is its source: an output, or a window.
- **wlr-screencopy frames count** when the client asks for a second frame
  of the **same output** within **2 s** of the previous one. They go on
  counting while a frame is pending (`copy_with_damage` waiting for a change
  on a still screen), and stop 2 s after the last frame once none is. grim
  captures several outputs at once, but each only once: it never counts.
- The **shell** is never reported (§4.2). Its captures are the desktop's
  own: previews that never leave the machine.

A count that ends is reported at once; the shell keeps the icon about a
second more (§6), as for the microphone, so that a portal that closes and
reopens its session doesn't make it blink.

## 4. Compositor

`compositor/src/capture_watch.c`, owned by the server like `a11y` and
`lock`.

### 4.1 `watch capture`

The command socket's generic `watch <topic>` (brightness.md §7.2) gets a
second topic. Every line is the whole state:

```json
{"clients":[
  {"pid":4242,"binary":"xdg-desktop-portal-wlr","app_id":"","window":"","portal":true,
   "targets":[{"type":"output","name":"DP-3","description":"Dell U2720Q"}]},
  {"pid":5151,"binary":"obs","app_id":"com.obsproject.Studio","window":"3f2a…","portal":false,
   "targets":[{"type":"window","app_id":"org.kde.konsole","title":"~ : zsh"}]}
]}
```

- `binary`: the basename of `/proc/<pid>/exe`. `app_id` and `window`: the
  app id and ext-foreign-toplevel identifier of the first window that client
  has, if any (OBS has one, wf-recorder doesn't).
- `portal`: the client is xdg-desktop-portal-wlr (its binary): the app is
  found in PipeWire (§5.2).
- `targets`: one per output or window, with no repeats. A window's title
  follows the window; an output's description is the one Settings shows.
- `{"clients":[]}`: nothing is captured.

`capture` is also part of `state` (the functional tests' snapshot).

### 4.2 The shell

The shell is the compositor's child (`server->shell`, `process.h`), started
through `/bin/sh -c`. A client is the shell when its pid is the child's, or
when its parent is the child and its binary is `vela-shell` (a shell that
didn't `exec`). Apps the shell launches are detached (`startDetached`): their
parent is not the shell, and they are reported like any other app.

The functional tests and Vela's recordings start the real shell as a client
of their own, not as the startup command: they name it with `test-shell
<pid>` on the command socket, which the compositor accepts only when every
backend is headless (like `test-output`). Today the shell's previews (Alt+Tab,
Task View, the source chooser) open one short session per image and would
never count anyway; this keeps the tests' shell the shell if that changes.

### 4.3 Recordings of Vela

Vela's own videos record a headless session with `vela-screen-record`, a
capture like any other. `scripts/recording_core.py` starts that compositor
with `VELA_CAPTURE_UNREPORTED=1`: nothing is reported, the topic always says
`{"clients":[]}`, and the log says so at start. An environment variable of
the compositor, not a `vela.conf` key: an app can't set it, and Settings
has nothing to offer for it.

### 4.4 Lifetime

- A session's record goes with the session (`destroy`); a screencopy
  frame's with its resource. A client that goes away takes everything with
  it.
- One timer for the whole module, set to the next moment the state could
  change (a session reaching 1 s, a screencopy stream's 2 s running out),
  and idle otherwise.
- Locking the session already revokes window sources
  (`vela_views_revoke_capture_on_lock`): those sessions end, and so does
  their report.

## 5. Shell model

`shell/src/screensharing.{h,cpp}`: `ScreenSharing` (`Sharing` in QML), a
client of `watch capture` like `Brightness`, reconnecting every second when
the compositor isn't there. `active`, `users` (name, icon, desktopId,
window, what) and `summary` (the tooltip); `active` stays true for a second
after the last capture ends.

### 5.1 Names

Each client becomes an entry with `name`, `icon` and the list of its
targets:

- **App**: its window's app id, else its binary, matched to a `.desktop`
  file as the mixers do (`AppModel::findDesktopId`, audio.md §5.2); else the
  binary itself.
- **Portal**: the apps of §5.2, each with the portal's targets. None known
  yet (PipeWire not there, or the app not linked yet): "An app".
- **Targets**: "your screen" with one output, else the output's
  description; "a window of <app>" (the window's app, named the same way),
  "a window" when it has none.

### 5.2 The app behind the portal

xdg-desktop-portal-wlr turns every share into a PipeWire node,
`media.class` `Video/Source`, `node.name` `xdpw-stream-XXXXXX`. The app that
receives it has a `Stream/Input/Video` node linked to it, with the same
identity properties as its audio streams (`application.process.binary`,
`pipewire.access.portal.app_id`...).

- `vela-audio`'s core already watches PipeWire's nodes and links. It also
  watches `Video/Source` and `Stream/Input/Video` nodes, and its snapshot
  gets `screenConsumers`: the apps whose video streams are linked to a
  portal node (`node.name` starting with `xdpw-stream-`), with their
  identity. Not running (paused) still counts: the share is open.
- `Audio::screenUsers` names them with the same resolver as the mixers.
- PipeWire can't tell which of several portal shares is whose: with more
  than one app, each is listed with all the portal's targets. Rare (two
  apps sharing through the portal at once), and still true about what is
  shared.

## 6. The indicator

`Taskbar.qml`, between the notification area and the microphone, on the
primary taskbar only, like the microphone:

- A screen with a small red dot (`monitor` from the icon theme, monochrome
  like the other system icons; the dot drawn by Vela). It appears as soon
  as a client is reported and stays about a second after the last one
  ends (`ScreenSharing`'s grace, §5).
- **Tooltip** after half a second (`TaskbarTooltip.qml`): "Firefox is
  sharing your screen", "… is sharing a window"; with more apps, "These
  apps are sharing your screen:" and their names.
- **Click**: a menu above the icon, like Caffeine's: "Screen sharing", then
  one row per app with its icon, its name, and on the right what it
  captures ("Your screen", "Dell U2720Q", "A window of Konsole"). Picking a
  row brings the app's window forward (`Tasks.showApp`, else the client's
  own window), where its "Stop sharing" is. When notifications are being
  held back (§7), a last part says so, with Settings > System >
  Notifications.

## 7. Notifications

- `notifications/silenceWhileSharing` in `vela-shell.conf`, **true** by
  default. Settings > System > Notifications: "Turn on do not disturb
  automatically while my screen is shared", under Do not disturb.
- `NotificationServer` gets `automaticDoNotDisturb` (set by the shell while
  `ScreenSharing` reports something and the setting is on) and `silenced`
  = `doNotDisturb || automaticDoNotDisturb`. Popups, the bell next to the
  clock and the notification center's switch follow `silenced`;
  `doNotDisturb`, the user's own, is never written by the rule.
- The notification center's header says "Silenced while your screen is
  shared" while the rule holds them back and the user's Do not disturb is
  off.
- Critical notifications still pop up, as with Do not disturb.
- The same property is where the automatic rule for fullscreen games, on the
  roadmap, will join (`automaticDoNotDisturb` is an OR of the rules).

## 8. Not in scope

- **Stopping a share from the panel.** The capture is the app's; ending it
  means killing the portal or the client, which can't be done honestly from
  the compositor. The app's own "Stop sharing" remains the way.
- **Hiding notifications from the capture** instead of from the user: the
  output source copies the composited frame, so leaving the notification
  layer out would need a second render per captured frame. Possible later,
  with its own measurements.
- **A permission prompt** for direct protocol clients (wf-recorder): Wayland
  has no standard for it yet (security-context-v1 would be the start).
- **Camera in use**: the same PipeWire watch could find it, but it is a
  separate indicator, for later.

## 9. Tests

| Test | What it proves |
|---|---|
| `test_screen_sharing.py`: a session held open on an output | reported after about 1 s with `type: output` and the output's name; gone when the session closes |
| … on a window | `type: window`, the window's app id and title |
| … a single screenshot (`vela-screen-dump`) | never reported |
| … wlr-screencopy frames in a row on one output | reported; gone 2 s after the last |
| … `copy_with_damage` on a still screen | still reported while its frame waits, past the 2 s |
| … one screencopy frame per output, at once (like grim) | never reported |
| … the shell's own captures (a fake shell that is the compositor's child) | never reported |
| … `VELA_CAPTURE_UNREPORTED=1` | never reported |
| … `test-shell <pid>` | that client's captures, even ongoing ones, stop being reported; a bad pid is ignored |
| `TheIndicator` (real shell) | the red dot appears, the tooltip and the menu open, the dot stays half a second after the end and then goes; Task View's previews never light it |
| … a client that dies while capturing | its report goes with it |
| `shell-screen-sharing` (unit) | JSON to entries: names from app ids and binaries, portal entries from PipeWire's apps, targets' wording, the second of grace |
| `shell-notification-silence` (unit) | the rule holds popups back, ORed with Do not disturb, which it never changes; the setting turns it off live |
| `audio` (private PipeWire) | a fake `xdpw-stream-` source and a fake app's video stream linked to it: `screenUsers` names the app; paused still counts; unlinked doesn't |
| `tst_notificationspage` (Settings) | the switch writes `notifications/silenceWhileSharing` |

Each test is checked by mutation: it must fail with the feature taken out.
By hand, in a real session: a share from Firefox and from Chromium (screen
and window), OBS through the portal, wf-recorder, a Spectacle screenshot.

## 10. Stages

| Stage | Content | Status |
|---|---|---|
| S1 | Compositor: `capture_watch.c`, `watch capture`, `state`, `VELA_CAPTURE_UNREPORTED`; `vela-share`; functional tests | done |
| S2 | Shell: `ScreenSharing`, the icon, its tooltip and menu; `test-shell`; unit and functional tests | done |
| S3 | PipeWire: `screenConsumers`, `Audio::screenUsers`, portal entries named; audio tests | to do |
| S4 | Notifications: the automatic rule, its switch in Settings, the center's header; tests | to do |
| S5 | `docs/configuration.md`, translations, a real-session test by hand | to do |
