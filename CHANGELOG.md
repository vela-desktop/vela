<!-- SPDX-FileCopyrightText: 2026 Brando Giuffrida -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Changelog

Notable changes in each Beta release, newest first. The full notes, with
packages and verification steps, are on the
[releases page](https://github.com/vela-desktop/vela/releases). Add a line
under "Unreleased" in the pull request that makes a user-visible change.

## Unreleased

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
