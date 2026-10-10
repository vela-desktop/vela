# Roadmap

This roadmap summarizes broad project milestones. Completed items indicate implemented functionality, not guaranteed stability or validation across every GPU and driver. For current compatibility limits, see [hardware coverage](hardware.md).

**Done**
- [x] **A real session**: login entry, systemd and D-Bus, portals, polkit, Xwayland,
  lock screen, idle, notifications, system tray.
- [x] **The look**: own Vulkan renderer, live acrylic blur, rounded corners and
  shadows, Vela's title bar, and modern context menus throughout the shell.
- [x] **A complete daily desktop**: Settings, Files, desktop icons, virtual desktops,
  Task View, Snap layouts and Snap Assist, light mode.
- [x] **Gaming and displays**: tearing control, VRR, direct scanout, multi-monitor
  hotplug with per-output scale.
- [x] **Daily comfort**: clipboard history, snipping tool, touchpad gestures, mouse and
  touchpad settings.
- [x] **Reliability**: compositor crash recovery, package and `vela-update`, CI.
- [x] **Languages**: English and Italian, switched live from Settings.

**Next**
- [ ] Input methods (`text-input`, `input-method`) and an emoji panel (Super+.).
- [ ] An Updates page in Settings, with a notification when a new version is out.
- [ ] Start menu: Recommended files, and search across files and settings.
- [ ] Screen recording in the snipping tool.
- [ ] More languages.
- [ ] Vela's own login screen.
- [ ] HDR and color management.
