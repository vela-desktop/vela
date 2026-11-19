# Taskbar

Design of the second pass on Vela's taskbar: which windows a button counts,
how an app asks for attention, what a click does on an app with several
windows, Super+1…9, the notification area's drawer, drag and drop onto
the taskbar, app badges, and the taskbar on any edge of the screen. Like the context menus' pass
([renderer.md §14](renderer.md#14-context-menus)), Windows is one reference
among KDE Plasma, GNOME, macOS and COSMIC, never the rule.

> **Status:** Implemented in PR #299. **Checked:** 19 November 2026.
>
> **Remaining:** real-session checks with LauncherEntry publishers and multiple
> physical displays (§15).

## 1. Goals

1. **What a button says is true.** The dash under an icon, the previews,
   the window count and a click all speak about the same windows.
2. **Never steal, never lose.** Windows still can't take the focus on their
   own (#264); when one wants it, the taskbar says so instead of dropping
   the request in silence.
3. **The keyboard reaches every app.** Super+1…9 and Super+0, as asked in
   [#214](https://github.com/vela-desktop/vela/issues/214) and in a comment
   on [#174](https://github.com/vela-desktop/vela/issues/174).
4. **The tray has an end.** A busy notification area folds into a drawer
   behind a button instead of scrolling, and the user picks what stays out.
5. **The user decides.** Every behavior added here has its setting, with a
   default that suits most people.

## 2. Decisions taken

| # | Decision |
|---|---|
| 1 | Buttons show the windows of the current desktop by default; "All desktops" is a choice. With the taskbar on every display, the apps can also be shown only on the display their window is on. |
| 2 | A window whose activation is refused, or that asks for attention, highlights its button. On by default, can be turned off. |
| 3 | Caffeine stays as it is: an icon of its own, always there (#83 put it there on purpose). |
| 4 | A click on the app in front, when it has several windows, opens its previews instead of switching blindly to the next window. The old behavior stays as a choice. |
| 5 | Settings > Personalization > Taskbar is reorganized around these settings (§12). |
| 6 | Super+1…9 and Super+0 open the apps on the taskbar, with Shift and Alt variants; they can instead switch virtual desktops, or be turned off; numbers show on the buttons while Super is held. |
| 7 | Each notification area icon is either on the taskbar or in a drawer behind a "^" button, chosen icon by icon: by dragging it, from its menu or in Settings. New icons go in the drawer. |
| 8 | A file dragged over an app's button brings its window forward, to drop the file into it; an app dragged onto the taskbar is pinned there. |
| 9 | Counts and progress that apps publish over `com.canonical.Unity.LauncherEntry` show on their buttons; each can be turned off. |
| 10 | The taskbar can sit on any edge of the screen: bottom (the default), top, left or right. One setting for every display. |

Prior work reused: Lowell137's [#67](https://github.com/vela-desktop/vela/pull/67)
(tray overflow, icons asking for attention stay out of it) and
[#68](https://github.com/vela-desktop/vela/pull/68) (LauncherEntry badges,
"99+", matching by desktop id with a short-name fallback), both closed on
17 October to wait for the redesign.

## 3. The taskbar today

From left to right: Start, Task View, the app buttons (pinned and running,
one per app, icons only, paged when space runs out), file transfers, the
notification area, screen sharing, microphone, Caffeine, the system icons
(network, volume, battery: quick settings), the clock with the
notification count, the show-desktop strip.

On an app button:

| Input | Does |
|---|---|
| Click | no window: launch; the app in front with one window: minimize; with several: next window; otherwise its most recent window |
| Middle click | a new instance |
| Wheel | the app's volume |
| Hover | previews of its windows on the current desktop |
| Right click / Shift+right click | jump list / window menu |
| Drag | reorder |

What this design fixes:

- `TaskbarModel` counts every window of the app on every desktop, while
  `TaskbarPreview.qml` keeps only the current desktop's
  (`Desktops.workspaceOf`). An app open only on desktop 2 shows the
  "running" dash on desktop 1, and its previews are empty.
- `compositor/src/activation.c` returns without a trace when it refuses an
  activation, both for Wayland tokens and for X11 requests.
- All taskbars show all apps, whatever display their windows are on.
- A large tray scrolls its icons (#266).

## 4. Which windows a button counts

### 4.1 Settings

| Setting | Values | Default |
|---|---|---|
| Show windows from | This desktop, All desktops | This desktop |
| Show apps on (only with the taskbar on all displays) | All taskbars, The taskbar where the window is open | All taskbars |

Windows, KDE and COSMIC default to the current desktop; macOS' Dock shows
every Space's apps. Windows and KDE both have a "display" filter.

### 4.2 Rules

A taskbar **shows** a window when it passes both filters:

- desktop: the window is on the current desktop, or on all of them
  (`workspaceOf` is -1), or the filter is "All desktops";
- display: the filter is "All taskbars", or the window's display is this
  taskbar's. A window's display is the one `vela_view_output()` gives,
  the same one snapping and fullscreen use.

Then, on each taskbar:

- **Pinned** apps always have their button. The dash, the previews and the
  window count speak only about shown windows.
- An **unpinned** app has a button only while it has a shown window.
- **A click** on a pinned app with no shown window, but windows elsewhere,
  goes to its most recent window, switching desktop if needed. It does not
  launch a second instance: single-instance apps (Telegram, Steam) would
  only raise the hidden window anyway, and the activation policy might
  refuse that. A middle click still opens a new instance.
- Jump list: "Close window" / "Close all windows" counts and closes the
  shown windows. "End task" still ends the process.
- Minimizing flies to the button on the taskbar that shows the window.
- A window asking for attention is shown whatever the filters (§5.3).

Alt+Tab, Task View and the Start menu are unchanged.

### 4.3 Where it lives

One `TaskbarModel` keeps every window, as now. Each taskbar gets a view of
it (`TaskbarView`, a proxy) with its display's name and both filters. The
view gives rows, window counts, the active state, and the windows that
activation, previews, closing and geometry act on. The model's methods
take the view's window list instead of the whole app's.

The compositor's workspaces state (`vela_workspaces_json`) gains each
window's display: `"outputs":{"<ext id>":"DP-1"}`, next to `windows`.
It is sent again when a window changes display.

## 5. Asking for attention

### 5.1 Sources

The compositor marks a view `attention` when:

1. an xdg-activation request names it but is refused (no valid token, or a
   token issued without the input serial and focused surface #264 asks
   for). GNOME and KDE treat that refusal the same way;
2. `vela_activation_request_x11()` refuses an X11 window;
3. an X11 window sets the urgency hint (`WM_HINTS` `XUrgencyHint`), or
   `_NET_WM_STATE_DEMANDS_ATTENTION` where wlroots exposes it.

The mark clears when the view gets the keyboard focus, or when an X11
window drops the hint. A request while the screen is locked still marks
the window, so it shows after unlocking. Wayland has no other urgency
request: xdg-activation is the way to ask.

The shell adds a fourth source: `urgent=true` in an app's LauncherEntry
(§10). It clears with `urgent=false` or when that app's sender leaves the
bus.

The compositor's workspaces state carries `"attention":["<ext id>", …]`,
sent again whenever it changes. wlr-foreign-toplevel has no such state.

### 5.2 How it looks

- The button's fill turns to `Theme.caution` (amber in the dark theme,
  brown-orange in light; a new `attention` token lets a theme choose its
  own). It pulses three times, 400 ms on and 400 ms off, then stays lit
  until the mark clears.
- With `animation-effects=no`, or while Do not disturb is on (the user's,
  or the one during screen sharing), it doesn't pulse: it is lit at once.
- The tooltip and `Accessible.name` add "needs attention".
- The dash and the badge are unchanged.

### 5.3 Filters

A window asking for attention has its button on every taskbar that would
show its app with "All desktops", even when the filters of §4 hide it. For
the display filter, that is only its own display's taskbar. A click
activates that window and switches desktop if needed. The button leaves
again when the mark clears, unless the app is pinned or has other shown
windows.

### 5.4 Setting

"Flash apps that need attention", on by default (`taskbar/attention`).
Off, nothing lights up. Windows still can't take the focus: the setting
never changes the activation policy.

## 6. A click on an app with several windows

| The app | Click |
|---|---|
| has no window shown | launch, or its most recent window elsewhere (§4.2) |
| is not in front | its most recent shown window (unchanged) |
| is in front, one window | minimize (unchanged) |
| is in front, several windows | **its previews** (default), or the next window (`taskbar/clickActive=next`) |

Windows and KDE open the previews; macOS brings every window forward. The
next window is the old behavior, kept as a choice.

Previews opened by a click stay open until a window is chosen, a click
lands outside, Esc is pressed, or the same button is clicked again. They
don't close when the pointer leaves, unlike hover previews. The keyboard
works in them: arrows move the highlight, Enter activates, Delete closes
the highlighted window. That is the same panel Super+N uses (§7.3).

## 7. Super+1…9 and Super+0

### 7.1 What the keys do

`super-number-keys` in `vela.conf`, read by the compositor:

| Value | Super+N | Super+Shift+N | Super+Alt+N |
|---|---|---|---|
| `apps` (default) | the Nth button (§7.2) | a new instance of it | its jump list |
| `desktops` | virtual desktop N | move the focused window to desktop N | — |
| `off` | — | — | — |

Super+0 is the tenth. A number beyond the buttons or the desktops does
nothing; `desktops` never creates a desktop.

Windows, GNOME and KDE give the numbers to the taskbar's apps (GNOME's
new window is Super+Ctrl+N); COSMIC and tiling compositors give them to
workspaces. None of the combinations is taken in Vela
([shortcuts.md](shortcuts.md)).

The keys are matched by **key code** (the row of digits, `KEY_1` …
`KEY_0`), not by symbol: on AZERTY the digits are Shift+key, and the
symbols there are `&é"'(`… The keypad doesn't count.

In a nested session the host keeps Super, and Alt+number is common in
apps (browser tabs, terminals), so there is no nested alternative.

The compositor treats them like Vela's other Super shortcuts: they work
over a fullscreen game too, and not on the lock screen.

### 7.2 Which button is N

The Nth button of the taskbar on the display with the focused window (or
the pointer, when nothing has the focus), counted in the order shown, in
that taskbar's view (§4). Start and Task View don't count. A button paged
out of view (§14.4 of renderer.md) counts as well, and is brought into
view. With `apps`, the compositor sends `taskbar-app <n> [new|jump]` to
the shell, which resolves the number. Only the shell knows the taskbar's
buttons.

### 7.3 Super+N, in detail

- No window shown: launch (or the most recent window elsewhere, §4.2).
- Not in front: its most recent shown window.
- In front, one window: nothing. Minimizing from the keyboard is
  Super+↓, and a second press shouldn't hide what the user just asked for.
- In front, several windows, with `clickActive=previews`: the previews
  open, the next window highlighted. **Pressing N again, with Super still
  held, moves the highlight; releasing Super activates it**, like Alt+Tab.
  Esc closes them. With `clickActive=next`: the next window.

### 7.4 Numbers on the buttons

While Super is held for 500 ms with no other key, the first ten buttons
show their number in a small pill at their lower left, like the clock's
notification count. They go when Super is released or another key is
pressed. A Super tap still opens Start on release, held or not. The
compositor sends `super-held` and `super-released` to the shell (only with
`apps`, and only while the taskbar is visible).

"Show numbers while holding Super" (`taskbar/numberHints`), on by default,
only with `apps`.

## 8. The notification area's drawer

### 8.1 Rules

- Each icon is **on the taskbar** or **in the drawer**, behind "^". The
  choice is the user's, icon by icon, and lasts across sessions:
  `taskbar/trayShown` lists the icons on the taskbar, in their order, by
  their StatusNotifierItem `Id` (the `Title` when an app has none). It
  replaces the count of icons (`taskbar/trayIcons`) of the first pass.
- An icon not in the list is in the drawer. That includes every **new**
  icon: an app that starts for the first time doesn't take room on the
  taskbar until the user puts it there. The drawer keeps the tray's order,
  the order the apps registered in.
- An icon whose status is `NeedsAttention` is on the taskbar while it asks,
  after the chosen ones, wherever it is set to be. A `Passive` icon (the app
  says it has nothing to show) is in the drawer while passive, even if
  chosen: "on the taskbar" means "on the taskbar when relevant", as KDE's
  "Shown when relevant".
- "^" sits before the tray icons, only when the drawer has something or
  while an icon is being dragged.
- When the bar is short of room, the last chosen icons go in the drawer for
  as long as it lasts, before the app buttons start paging. This **replaces
  the tray's scrolling** from #266. App buttons keep their paging.
- Only the main display's taskbar has the notification area, as now.

### 8.2 Moving an icon

- **Dragging.** An icon dragged from the taskbar onto "^" (or onto the open
  drawer) goes in the drawer. One dragged from the drawer onto the taskbar
  comes out, where it is dropped: a marker shows the place. Dragged along
  the taskbar, it changes place among the chosen icons. A drag needs the
  same 8 pixels as a button's; a short press is still a click.
- **The icon's menu.** After the app's own entries, the shell adds a
  separator and **Hide in the drawer** or **Show on the taskbar**. An app
  without a menu of its own gets a menu with only that entry on right
  click.
- **Settings.** Settings > Personalization > Taskbar > Notification area
  lists the icons of the apps running now, with a switch each (on: on the
  taskbar). It reads them from the StatusNotifierWatcher the shell owns,
  over D-Bus, and says that the icons of apps not running show once they
  start. Settings and the shell write the same key; the shell follows its
  changes at once.

### 8.3 The drawer

A panel next to "^", like the previews (visual coherence's `flyout`,
[§6.9](visual-coherence.md)). It holds a grid of icons, four columns, as
tall as the screen allows and scrolling past it. Each icon behaves as on
the taskbar: click, middle click, right click for the app's menu, tooltip.
Choosing an entry or activating an icon closes it, as do a click outside
and Esc. Arrows and Enter work in it, and the selected icon stays in view.

## 9. Drag and drop onto the taskbar

### 9.1 A file over a button

During any drag (from Files, the desktop, a browser: `wl_data_device`),
the pointer resting 600 ms over an app's button:

- with one shown window: brings it forward;
- with several: opens the previews, and resting 600 ms over one brings
  that window forward;
- with none: nothing (the button highlights only).

The drag goes on: the user carries it into the window and drops it there.
**A drop on a button, or on the bar, is refused**, so the source keeps its
file, except an app being pinned (§9.2). Windows, KDE and macOS (spring
loading) bring windows forward the same way.

### 9.2 Pinning an app by dragging it

- What counts as an app: a `.desktop` file (`text/uri-list`) that is an
  installed application or a launcher file, from Files or the desktop. Also
  an app dragged from Start: Start's app tiles become drag sources,
  carrying the same `.desktop` path.
- Over the app buttons, a marker shows where it lands; the drop pins it
  there (`TaskbarModel::pin` at an index). An app already pinned moves to
  that place.
- Anything else is refused.

There is no setting: neither behavior acts without the user dragging.

## 10. Badges and progress

### 10.1 The interface

The shell listens on the session bus for the signal
`com.canonical.Unity.LauncherEntry.Update(s app_uri, a{sv} properties)`
from any sender and any path. Apps already send it: Telegram, Discord,
Thunderbird and Steam for counts, Firefox and KDE apps for progress.
`app_uri` is `application://<desktop id>`. Properties read:

| Property | Type | Use |
|---|---|---|
| `count`, `count-visible` | int64, bool | the number |
| `progress`, `progress-visible` | double 0–1, bool | the bar |
| `urgent` | bool | attention (§5.1) |

`quicklist` is ignored. An entry is dropped when its sender leaves the bus.
An app is matched by desktop id. When that fails, and one of the two ids is
a shorthand without dots, the last dotted part of the other is compared with
it (`discord` and `com.discordapp.discord`), as in #68. Two different
qualified ids (`org.example.Mail`, `org.other.Mail`) never match.

To verify with real apps before the work is merged: whether some only send
when `com.canonical.Unity` is owned on the bus, or when
`XDG_CURRENT_DESKTOP` names a desktop they know. Also, which Flatpak apps
have `--talk-name=com.canonical.Unity`. #68 owned the name. The real
behavior decides.

### 10.2 How it looks

- **Count**: 1 to 99, then "99+", in a pill at the icon's upper right,
  with the clock's notification count's look. 0, or `count-visible=false`,
  shows nothing.
- **Progress**: the dash under the icon becomes a track as wide as the
  button less 8 units, filled in the accent from the left. When
  `progress-visible` goes false, it goes back to the dash.
- A badge stays after the app's windows close, for as long as the sender
  stays on the bus.

### 10.3 Settings

"Show badges on apps" (`taskbar/badges`) and "Show progress on apps"
(`taskbar/progress`), both on by default. There is no choice app by app
(Windows has none; macOS's is in its notifications).

## 11. Position on the screen

### 11.1 The setting

"Taskbar position": Bottom, Top, Left or Right (`taskbar/position`,
default `bottom`), in Taskbar behaviors. It applies to the taskbar of every
display. KDE and COSMIC let each panel pick its edge, macOS' Dock has
three; one choice for every display keeps a single picture of where things
are, and a taskbar per display is already a choice of its own.

### 11.2 Layout

- **Top** mirrors bottom: the same row, the hairline on the edge facing the
  windows, panels opening downwards.
- **Left and right**: a column as wide as the taskbar is tall (48, a style's
  `taskbar-height`), never less than 56 so the clock fits. From the top:
  Start, Task View, the app buttons (paged up and down when they don't
  fit); at the bottom: file transfers, the notification area (one icon per
  row, "^" above them), screen sharing, microphone, Caffeine, the system
  icons (one under the other), the clock, the show-desktop strip (8 high,
  at the bottom end).
- "Alignment" keeps its two values: Center centers the apps along the bar;
  Left puts them at its start, which is the top on a vertical bar. Settings
  names it **Top** there.
- The clock on a vertical bar: the time on one line (with the seconds when
  shown), AM/PM on a line of its own with a 12-hour clock, the date as day
  and month under it. The notifications' count sits at its top corner.
- A button's dash, its progress track and the attention highlight sit on
  the side of the screen's edge: under the icon at the bottom, above it at
  the top, left of it on a left bar, right of it on a right bar. The count
  badge stays at the icon's corner away from the edge.
- Floating styles keep their margins and radius on every edge. On the left
  and right, a pill that fits its apps (`taskbar-fit`), a split tray
  (`taskbar-split-gap`) or the tray at the screen's edge (`taskbar-edge`)
  becomes one pill along the whole edge: those layouts are drawn for a row.

### 11.3 What opens from it

Every panel opens from the taskbar's edge, towards the middle of the
screen:

| Panel | Bottom | Top | Left | Right |
|---|---|---|---|---|
| Start | above Start (centered or at the left, as now) | below Start | right of the bar, at the top | left of the bar, at the top |
| Quick settings, notification center, clipboard history | bottom right | top right | bottom left | bottom right |
| Notification popups | bottom right | top right | bottom left | bottom right |
| Previews, tooltips, the drawer, jump lists and the taskbar's menus | above the button | below it | right of it | left of it |
| Volume indicator | above the bar, centered | below it | bottom center | bottom center |

They slide in from the bar's side. The desktop icons and Task View keep out
of the bar's space as now; the compositor already reserves an exclusive
zone on any edge, so maximized and snapped windows follow. A tray icon is
dragged off the bar towards the middle of the screen to go in the drawer,
whichever edge that is.

## 12. Settings > Personalization > Taskbar

| Card | Settings |
|---|---|
| Taskbar items | Task view |
| Notification area | **The icons of the running apps**, a switch each (§8.2) |
| Taskbar behaviors | **Taskbar position** (Bottom, Top, Left, Right); Alignment (Center, Left or Top); **Show windows from**; Show the taskbar on all displays, with **Show apps on** under it (disabled while off); **When I click the app in front with several windows** (Show its windows / Switch to the next one); Show desktop from taskbar corner; End task |
| Badges and attention | **Flash apps that need attention**; **Show badges on apps**; **Show progress on apps** |
| Keyboard | **Super+1…9** (Taskbar apps / Virtual desktops / Off); **Show numbers while holding Super** (disabled unless Taskbar apps) |
| Clock | Show the date; Show seconds |
| Styles | Taskbar; Quick settings and notification center |
| Related settings | Time format; Notifications |

New keys (`vela-shell.conf`, unless said):

| Key | Default | Meaning |
|---|---|---|
| `taskbar/windowsFrom` | `desktop` | `desktop` or `all` (§4) |
| `taskbar/appsOn` | `all` | `all` or `window` (§4) |
| `taskbar/clickActive` | `previews` | `previews` or `next` (§6) |
| `taskbar/attention` | `true` | Highlight apps asking for attention (§5) |
| `taskbar/trayShown` | empty | The tray icons on the taskbar, in order (§8) |
| `taskbar/position` | `bottom` | `bottom`, `top`, `left` or `right` (§11) |
| `taskbar/badges` | `true` | Counts on buttons (§10) |
| `taskbar/progress` | `true` | Progress on buttons (§10) |
| `taskbar/numberHints` | `true` | Numbers while holding Super (§7.4) |
| `super-number-keys` (`vela.conf`) | `apps` | `apps`, `desktops` or `off` (§7) |

Unknown values fall back to the default. Settings writes them at once, and
keeps the saved value shown when a write fails (#218).

## 13. Stages

One PR, from `origin/main`, one commit per point:

0. This document.
1. The Settings page reorganized around today's entries (point 5).
2. Which windows a button counts: `TaskbarView`, `outputs` in the
   workspaces state, the two settings (point 1).
3. Click on the app in front: previews kept open, keyboard in the previews
   (point 4).
4. Attention: the compositor's mark, `attention` in the state, the button,
   the setting (point 2).
5. Super+1…9: the compositor's keys, `taskbar-app`, `super-held`, number
   hints, the settings (point 6; it needs 2 and 3).
6. Tray overflow: "^", the flyout, `Passive`/`NeedsAttention`, no more
   tray scrolling (point 7).
7. Drag and drop: over buttons, pinning, Start's tiles as drag sources
   (point 8).
8. Badges and progress: `LauncherEntry`, the settings, `urgent` into
   attention (point 9).
9. Italian translations.
10. Docs: [renderer.md §14.4](renderer.md#144-taskbar-an-app-button-jump-list)
    (the tray's scrolling, links here), [features.md](features.md),
    [shortcuts.md](shortcuts.md), [configuration.md](configuration.md),
    `CHANGELOG.md`.
11. The drawer chosen icon by icon: `trayShown`, dragging, the menu entry,
    the Settings list (point 7, second pass).
12. The taskbar on any edge: the setting, the bar's layout, the panels
    (point 10).

## 14. Not in scope

- Auto-hide, labeled or uncombined buttons: the [roadmap](roadmap.md)'s
  later work.
- A different edge on each display (§11.1).
- Super+T and Super+B to reach the taskbar or the tray with the keyboard.
- Badges app by app.
- Opening a dropped file with the app it is dropped on, or pinning it to
  the app's jump list.
- Caffeine (§2, decision 3), Task View, Alt+Tab.

## 15. Tests

The scenarios below are the validation plan. Automated checks cover refused
Wayland activation, X11 urgency and focus, settings, tray layout, D-Bus
publisher cleanup, and headless taskbar shortcuts and drag and drop. The
real-session checks at the end use the listed apps and physical displays.

- **TaskbarView** (C++): the filters, pinned and unpinned apps, sticky
  windows, a window moving desktop and display, attention overriding the
  filters, activation switching desktop.
- **Compositor** (CPU tests): a refused Wayland token and a refused X11
  request mark the view; focus clears it; the urgency hint; `outputs` and
  `attention` in the workspaces state; `super-number-keys` in each mode,
  matched by key code under an AZERTY keymap; `super-held` after 500 ms
  and not after a Super tap.
- **QML** (`tst_*`): the attention pulse and its stillness without
  animation or with Do not disturb; previews kept open after a click and
  their keyboard; number hints; the tray's split by `trayShown`, `Passive`
  and `NeedsAttention`, and shrinking for room; an icon dragged into and out
  of the drawer; the bar's layout and the panels' sides on each edge; the badge's "99+" and the
  progress track.
- **LauncherEntry** (C++): payload parsing, an app's sender leaving the
  bus, the short-name match.
- **Settings** (`preferences_test`, `tst_*`): each key read and written,
  unknown values, the disabled states.
- **Functional, headless** (`functional-shell`): Super+N on a pinned app,
  a running one and one with several windows; Super+Shift+N; a drag from
  Files over a button to the window; an app dragged from Start to the bar;
  a fake LauncherEntry sender; an X11 client asking for attention; the
  reserved space and a maximized window with the bar on each edge.
- **By hand, in a real session**: Telegram, Discord, Thunderbird, Steam and
  Firefox badges (§10.1); a chat app asking for attention over a
  fullscreen game; two displays.
