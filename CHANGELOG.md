<!-- SPDX-FileCopyrightText: 2026 Brando Giuffrida -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Changelog

Notable changes in each Beta release, newest first. The full notes, with
packages and verification steps, are on the
[releases page](https://github.com/vela-desktop/vela/releases). Add a line
under "Unreleased" in the pull request that makes a user-visible change.

## Unreleased

- Keyboard settings have their own section in Settings, with Layouts and
  Typing pages. When no layout is chosen, Settings shows the one in use and
  where it comes from (the system, or the XKB_DEFAULT_* variables). Vela no
  longer takes the layout from KDE's settings.

- Adding a keyboard layout lists every layout the system has, including
  EurKEY, APL, the phonetic alphabet and the layouts in your own XKB rules,
  grouped with their variants, with their country's flag, named in Vela's
  language and found by name, language or country. Settings allows up to
  four layouts, as many as Vela can switch between.

- Before adding a layout, Settings draws its keyboard (ISO, ANSI, JIS or
  ABNT2) with every level's characters and lets you type with it in a
  test field, dead keys and Compose included, without changing the layout
  in use. Installed layouts have the same Preview.

- Settings > Keyboard > Typing chooses what the Caps Lock key does (Ctrl,
  Esc, Backspace or nothing), a Compose key, swapped Ctrl and Alt or Alt
  and Win, and a numeric keypad that always types digits. Other XKB
  options in vela.conf are kept. The system's XKB options (localectl) now
  apply with layouts chosen in Settings too, until Settings changes one.

- Num Lock can be on when Vela starts, off, or as it was at the end of the
  last session (Settings > Keyboard > Typing, as asked in #214). Caps Lock
  and Num Lock are now shared by every keyboard, and changing the layout
  no longer turns them off.

- The lock screen says when Caps Lock is on, under the password field, and
  the authorization dialog now reads Caps Lock's real state instead of
  guessing it from the letters typed.

- Turning Caps Lock on or off shows *Caps Lock on* or *Caps Lock off* at
  the bottom of the screen for a moment, on the lock screen too, unless
  the key does something else. *Show Caps Lock on screen* in Settings >
  Keyboard > Typing turns it off.

- Settings > Keyboard > Custom shortcuts: your own key combinations to
  open an app, run a command, or open a link or file. Recording catches
  Win and Vela's combinations; Vela's and the reserved ones are refused,
  and those without Win need a confirmation, since apps use them.

- Shortcut modifiers are now exact, so Win+Shift+E no longer opens Files
  and Win+Shift+L no longer locks: those combinations are free for apps
  and custom shortcuts. Letters follow the layout in use (Win+Z where the
  Z is printed, on AZERTY too) and work with a Russian or Greek layout
  active; Print alone opens the Snipping Tool.

- *Open Start with the Win key* in Settings > Keyboard > Typing: turned
  off, the Win key by itself no longer opens Start, while Win shortcuts,
  the taskbar numbers and the Start button keep working.

- Closing a laptop lid with an active external display temporarily turns off
  the built-in panel. Reopening the lid restores its display settings.
  System suspend on lid close follows the system's logind policy.

- The Ubuntu package recommends the GTK and wlr portal backends, so file
  choosers in sandboxed apps, screenshots and screen sharing work after a
  normal installation.

- Display layouts keep monitors from overlapping. Unplugging a monitor closes
  gaps that would otherwise trap the pointer on one screen.

- The taskbar filters windows by desktop and display, opens keyboard-accessible
  previews on click, and adds Super+number shortcuts, attention indicators,
  app badges and progress, a tray drawer chosen icon by icon (drag an icon in
  or out of "^", its menu, or Settings), and drag-to-pin and drag-to-raise.
  The taskbar can sit at the bottom, the top, the left or the right of the
  screen (Settings > Personalization > Taskbar > Taskbar position).

- Context menus: the entries apps add (KDE service menus) are in the menu
  itself, with no "Show more options"; Settings > Personalization > Context
  menu hides the entries it lists, menu by menu. Start shows one menu for every app, the window menu
  can move a window to another desktop, and commands have monochrome icons.
- Run (Win+R) is gone, with three Win+X entries that opened often-missing KDE
  tools and the network icon's always-disabled troubleshooter.

## 0.1.2 — 16 November 2026

- Fedora 45 packages, beside Fedora 44.
- Settings > System chooses KWallet or GNOME Keyring for app passwords; a
  dialog asks at login until one is chosen. KWallet starts through ksecretd.
- Background chat apps no longer steal focus; the taskbar handles overflowing
  buttons and tray icons; Start keeps its grid; X11 windows get their final
  positions.
- Settings reports failed saves and keeps the previous preferences.
- More allocation failures recover instead of ending the desktop; Vulkan
  allocations from clients and the icon cache have budgets.
- Stricter checks on the command socket, privileged protocols, client buffers
  and configuration paths; the lock survives compositor crashes.
- Linux 6.5 or later is required.
- The updater shipped up to this release only finds prereleases. Beta releases
  are now normal releases. For the first update, run
  `sh packaging/vela-update --channel beta` from the root of an up-to-date
  source checkout
  ([details](docs/release-channels.md#updating-older-beta-installations)).

[All changes](https://github.com/vela-desktop/vela/compare/v0.1.1...v0.1.2)

## 0.1.1 — 3 November 2026

- Volume wheel, keys and knobs no longer stick on devices that report a
  slightly different volume.
- A resize starts only on a window border you can see.
- Hover and cursor update when a window opens, closes or moves under a still
  pointer.
- The Ubuntu package is 2.7 MiB instead of 78 MiB.

[All changes](https://github.com/vela-desktop/vela/compare/v0.1.0...v0.1.1)

## 0.1.0 — 31 October 2026

The first Beta: Vela's own Wayland compositor and Vulkan 1.4 renderer, a
Qt Quick shell, Files and Settings, in English and Italian, for Arch,
Fedora 44 and Ubuntu 26.10.

- Window management: snap layouts and Snap Assist, snap groups, Task View
  with virtual desktops, Alt+Tab with live previews.
- Settings > System > Updates with the Beta and Experimental channels.
- Native Bluetooth pairing through `vela-bluetooth`.
- X11 apps at native resolution, direct scanout for fullscreen games.
- Recovery from compositor crashes (apps reconnect, the lock holds) and from
  Vulkan device loss.
- Signed packages, verified with Cosign before installation.
