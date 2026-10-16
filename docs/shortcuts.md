# Vela keyboard shortcuts

Vela uses Super for desktop-level actions in a normal session. When Vela runs nested inside another Wayland desktop, the host usually owns Super, so Vela enables Alt-based alternatives for the shortcuts that would otherwise be unreachable.

Those nested alternatives are active only in nested mode; in a normal Vela session, ordinary Alt+letter combinations remain available to applications.

## Windows and multitasking

| Action | Normal session | Nested session |
|---|---|---|
| App launcher | <kbd>Super</kbd> | <kbd>Alt</kbd>+<kbd>S</kbd> |
| Task View | <kbd>Super</kbd>+<kbd>Tab</kbd> | <kbd>Alt</kbd>+<kbd>W</kbd> |
| Switch windows | <kbd>Alt</kbd>+<kbd>Tab</kbd> | <kbd>Alt</kbd>+<kbd>J</kbd> |
| Snap left / right | <kbd>Super</kbd>+<kbd>←</kbd> / <kbd>→</kbd> | <kbd>Alt</kbd>+<kbd>←</kbd> / <kbd>→</kbd> |
| Maximize / restore, then minimize | <kbd>Super</kbd>+<kbd>↑</kbd> / <kbd>↓</kbd> | <kbd>Alt</kbd>+<kbd>M</kbd> |
| Snap layouts | <kbd>Super</kbd>+<kbd>Z</kbd> | <kbd>Alt</kbd>+<kbd>Z</kbd> |
| Previous / next desktop | <kbd>Super</kbd>+<kbd>Ctrl</kbd>+<kbd>←</kbd> / <kbd>→</kbd> | <kbd>Alt</kbd>+<kbd>Ctrl</kbd>+<kbd>←</kbd> / <kbd>→</kbd> |
| New desktop | <kbd>Super</kbd>+<kbd>Ctrl</kbd>+<kbd>D</kbd> | <kbd>Alt</kbd>+<kbd>Ctrl</kbd>+<kbd>D</kbd> |
| Close desktop | <kbd>Super</kbd>+<kbd>Ctrl</kbd>+<kbd>F4</kbd> | — |
| Show desktop (also: the strip at the right edge of every taskbar) | <kbd>Super</kbd>+<kbd>D</kbd> | <kbd>Alt</kbd>+<kbd>D</kbd> |
| Move any window | <kbd>Super</kbd>+drag | — |
| Resize any window | <kbd>Super</kbd>+right-drag | — |
| Window menu | <kbd>Alt</kbd>+<kbd>Space</kbd> | <kbd>Alt</kbd>+<kbd>Space</kbd> |
| Close window | <kbd>Alt</kbd>+<kbd>F4</kbd> | <kbd>Alt</kbd>+<kbd>Q</kbd> |

## Shell and apps

| Action | Normal session | Nested session |
|---|---|---|
| Files | <kbd>Super</kbd>+<kbd>E</kbd> | <kbd>Alt</kbd>+<kbd>E</kbd> |
| Settings | <kbd>Super</kbd>+<kbd>I</kbd> | <kbd>Alt</kbd>+<kbd>I</kbd> |
| Quick settings | <kbd>Super</kbd>+<kbd>A</kbd> | <kbd>Alt</kbd>+<kbd>A</kbd> |
| Notification center | <kbd>Super</kbd>+<kbd>N</kbd> | <kbd>Alt</kbd>+<kbd>N</kbd> |
| Clipboard history | <kbd>Super</kbd>+<kbd>V</kbd> | — |
| Sound output and volume mixer | <kbd>Super</kbd>+<kbd>Ctrl</kbd>+<kbd>V</kbd> | — |
| Snipping tool | <kbd>Super</kbd>+<kbd>Shift</kbd>+<kbd>S</kbd> or <kbd>PrtSc</kbd> | <kbd>PrtSc</kbd> |
| Run | <kbd>Super</kbd>+<kbd>R</kbd> | <kbd>Alt</kbd>+<kbd>R</kbd> |
| Power-user menu | <kbd>Super</kbd>+<kbd>X</kbd> | <kbd>Alt</kbd>+<kbd>X</kbd> |
| Terminal | <kbd>Super</kbd>+<kbd>Enter</kbd> | <kbd>Alt</kbd>+<kbd>Enter</kbd> |
| Lock | <kbd>Super</kbd>+<kbd>L</kbd> | <kbd>Alt</kbd>+<kbd>L</kbd> |

## Input and accessibility

| Action | Shortcut |
|---|---|
| Next keyboard layout | <kbd>Super</kbd>+<kbd>Space</kbd> |
| Magnifier in | <kbd>Super</kbd>+<kbd>+</kbd> |
| Magnifier out | <kbd>Super</kbd>+<kbd>-</kbd> |
| Magnifier off | <kbd>Super</kbd>+<kbd>Esc</kbd> |
| Toggle color filters | <kbd>Super</kbd>+<kbd>Ctrl</kbd>+<kbd>C</kbd> |

The color-filter shortcut must be enabled in Accessibility settings (`color-filters-shortcut=yes`).

## Media and volume

The keyboard's media and volume keys (and a volume knob) work whatever window has the focus, even on the lock screen. Play/pause, stop, next and previous go to the media player that is playing (MPRIS), otherwise to the one that played last. Volume up and down move by 2 points, so the number stays even, unless the output only has a few positions (a Bluetooth headset in call mode), which it then takes one at a time; held down, they repeat at the keyboard's repeat delay and rate. The volume indicator appears at the bottom of the screen. The wheel over the taskbar's volume icon does the same, and over an app's taskbar button it changes that app's volume only. The microphone mute key turns the default microphone off or on; its indicator says which once the system has confirmed it.

| Action | Shortcut |
|---|---|
| Play / pause, stop | <kbd>⏯</kbd>, <kbd>⏹</kbd> |
| Previous / next track | <kbd>⏮</kbd> / <kbd>⏭</kbd> |
| Volume down / up | <kbd>🔉</kbd> / <kbd>🔊</kbd>, or the knob |
| Mute | <kbd>🔇</kbd> |
| Microphone on / off | the keyboard's microphone mute key |

## Back and Forward in Vela's apps

File Explorer, Settings and quick settings' pages go back and forward the same way, whatever has the focus: the mouse's side buttons, the keyboard's Back/Forward keys and <kbd>Alt</kbd>+<kbd>←</kbd> / <kbd>→</kbd>. Every Vela app gets the side buttons from `common/` (it links `vela-app`): they become Qt's Back and Forward commands, so an app only handles `StandardKey.Back` and `StandardKey.Forward`. Pressed on a window that isn't active, the button activates it and then goes back, like on Windows.

## Session and system

| Action | Shortcut |
|---|---|
| Switch virtual console | <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>F1</kbd> … <kbd>F12</kbd> |
| Quit Vela | <kbd>Alt</kbd>+<kbd>Shift</kbd>+<kbd>Esc</kbd> |

## Touchpad gestures

The exact behavior can be configured in Vela Settings.

By default, Vela supports three- and four-finger gestures for desktop-level navigation, including Task View, showing the desktop and switching apps or virtual desktops.

See [configuration.md](configuration.md) for the corresponding `vela.conf` keys.
