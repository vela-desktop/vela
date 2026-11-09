# Features

What Vela 0.1 does today, area by area. The [README](../README.md) keeps only the
overview; this page is the detailed list and changes as the desktop evolves. For the
complete key reference see [keyboard shortcuts](shortcuts.md), and for what is planned
see the [roadmap](roadmap.md).

## Screenshots

<table>
<tr>
<td width="50%"><img src="screenshots/snap.webp" alt="File manager and Settings snapped side by side"><br><sub><b>Snap</b>: windows side by side with Super+arrows or by dragging to an edge or corner.</sub></td>
<td width="50%"><img src="screenshots/task-view.webp" alt="Task View with two virtual desktops"><br><sub><b>Task View</b> (Super+Tab): live window previews and virtual desktops.</sub></td>
</tr>
<tr>
<td width="50%"><img src="screenshots/files.webp" alt="The Vela file manager showing a Pictures folder"><br><sub><b>Files</b>: tabs, breadcrumbs, thumbnails and previews; first frame in about 100 ms.</sub></td>
<td width="50%"><img src="screenshots/alt-tab.webp" alt="Alt+Tab switcher with window previews"><br><sub><b>Alt+Tab</b> with previews, minimized windows included.</sub></td>
</tr>
<tr>
<td width="50%"><img src="screenshots/light.webp" alt="Vela in light mode"><br><sub><b>Light and dark</b> modes for the shell and for apps, switched instantly.</sub></td>
<td width="50%"><img src="screenshots/lock.webp" alt="The Vela lock screen"><br><sub><b>Lock screen</b>: built on ext-session-lock, so a crash never reveals the desktop.</sub></td>
</tr>
</table>

<table>
<tr>
<td width="34%" valign="top"><img src="screenshots/snap-layouts.webp" alt="Snap layouts flyout over the maximize button"><br><sub><b>Snap layouts</b>: hover the maximize button or press Super+Z.</sub></td>
<td width="33%" valign="top"><img src="screenshots/context-menu.webp" alt="Vela desktop context menu"><br><sub><b>Context menus</b> with a compact primary view and KDE service menus under "Show more options".</sub></td>
<td width="33%" valign="top"><img src="screenshots/panels.webp" alt="Quick settings and the notification center"><br><sub><b>Quick settings</b> (Super+A) and the <b>notification center</b> (Super+N).</sub></td>
</tr>
</table>

## Window management and multitasking
- **Snap layouts**: halves, quarters, and six layouts (thirds, 2/3 + 1/3,
  and more). **Snap Assist** offers your other windows for the empty space. Windows you
  arrange this way form a **snap group** that the taskbar shows and restores together.
- **Task View and virtual desktops** (Super+Tab): drag windows between desktops,
  rename and reorder desktops, and switch with Super+Ctrl+←/→ and a sliding animation.
  Your desktops are remembered across sessions.
- **Alt+Tab** with live previews, including minimized windows. A quick Alt+Tab just
  switches without showing the panel.
- **Window animations**: windows fade and rise as they open, shrink away as they close,
  and fly into their taskbar button when minimized.
- **Rounded corners and soft shadows**, drawn by the compositor and sharp at every
  scale. They turn off when a window is maximized, snapped or fullscreen.
- **Vela's own title bar** for apps that accept server-side decorations (Qt, KDE, X11),
  with the app icon and a wallpaper-tinted translucent surface. Apps that draw their own
  title bar, such as Chromium with tabs on top, keep theirs.
- Invisible resize borders, Super+drag to move, Super+right-drag to resize, and a window
  menu (Alt+Space) with keyboard move and resize.

## Shell
- **Taskbar**: pinned and running apps centered (or left-aligned), drag to reorder, jump
  lists with recent files, **hover previews**, a system tray (StatusNotifierItem), and
  one taskbar per monitor.
- **App launcher**: instant search, pinned apps, keyboard navigation, and a Super+X power-user menu.
- **Quick settings** (Super+A): Wi-Fi network picker, Bluetooth, airplane mode, power
  mode, Night light, accessibility, brightness, volume and battery, and a sound page
  (Super+Ctrl+V) to pick the output, see Bluetooth headsets' charge, and set each app's
  volume and output.
- **Media, volume and microphone keys** that work whatever has the focus, with an
  indicator; the wheel over an app's taskbar button sets that app's volume.
- **Notifications** with actions, a **notification center** with a calendar (Super+N),
  and Do Not Disturb.
- **Clipboard history** (Super+V): text and images, pinned items that survive a reboot,
  and one-click paste into the focused app.
- **Snipping tool** (Super+Shift+S or PrtSc): capture a rectangle, a window or a full
  screen, straight to the clipboard and to `Pictures/Screenshots`.
- **Desktop icons** with thumbnails, drag and drop to and from apps, Properties, and
  undo. Also **Run** (Super+R) and **Show desktop** (Super+D).
- **Light and dark** modes and a coordinated accent palette, applied to KDE and GTK apps
  too.

## Files (`vela-files`)
- Tabs, breadcrumb address bar, search in subfolders, Details and icon views (remembered
  per folder), and a **preview pane** (images, text, video and PDF thumbnails).
- Background copy and move with progress and conflict resolution, drag and drop
  everywhere, ZIP/7z/TAR, shortcuts, Favorites, Trash with restore.
- **Atomic file replacement and staged copies.** Each file is written to a hidden temporary
  next to its destination and renamed into place only once complete. Replacing a file keeps
  the previous contents until the new copy is safely written, so a full disk or read error
  does not turn a good destination file into a partial one.
- **Unmounted drives** (udisks) in This PC: double-click to mount, eject from the menu.

## Settings (`vela-settings`)
- **Displays**: drag-and-drop arrangement, scale, resolution, refresh rate, rotation, and
  "Keep these settings?" with compositor-owned revert on timeout or a Settings crash.
  Native Wayland configuration requires no external display utility. Also **Night light**, variable refresh
  rate and tearing.
- **Sound, Notifications, Power, Clipboard**, **mouse and touchpad** (tap to click,
  scroll direction, three- and four-finger gestures), and Network.
- **Bluetooth**: Vela's own `vela-bluetooth` daemon pairs, connects and forgets
  devices, and answers pairing questions (PIN, passkey, confirmation) from the shell,
  even after Settings is closed ([design](bluetooth.md)).
- **Updates**: the installed and newest version, what's new, the Beta or Experimental
  channel, and an update that runs in a terminal. Vela checks in the background and
  sends a notification when an update is available ([design](updates.md)).
- **Personalization**: wallpaper, colors, taskbar. **Apps**: installed and default apps.
  **Time and language**: date and time, keyboard layouts.
- **Accessibility**: magnifier, color filters, sticky keys.

## Graphics and gaming
- Vela's **own scene graph and Vulkan 1.4 renderer**: no `wlr_scene`, no GL fallback.
- **Live acrylic blur** (dual Kawase) behind the shell, exposed to apps through the
  standard `ext-background-effect-v1` protocol.
- **Direct scanout** for fullscreen apps, **VRR** (FreeSync/G-Sync, automatic for
  fullscreen apps by default), **tearing control** for games, explicit sync
  (`linux-drm-syncobj-v1`), relative pointer and pointer constraints.
- **Night light** in the monitor's gamma LUT, so screenshots stay untinted and games keep
  direct scanout. It can follow sunset and sunrise, computed offline from your time zone.
- Picks the **highest refresh rate** at native resolution, even when the monitor
  advertises 60 Hz as "preferred".
- **Xwayland** on demand for Steam, games and older apps. X11 apps run 1:1 in the
  monitor's pixels, so a fullscreen game gets the native resolution even at 125%, stays
  sharp and can use direct scanout; `Xft.dpi` follows the scale.

## Accessibility and input
- Magnifier (Super+Plus), grayscale and color-blindness filters (Super+Ctrl+C), and sticky
  keys.
- Familiar touchpad defaults and gestures: three or four fingers up for Task View,
  down for the desktop, sideways to switch apps or desktops.

## A session you can rely on
- **Crash recovery.** A tiny supervisor holds the Wayland socket. If the compositor
  crashes, a new one starts on the same socket and Qt/KDE apps reconnect and reappear,
  as with KWin. If the screen was locked, Vela comes back **locked**. Three crashes in a
  minute end the session cleanly.
- If the shell crashes, the compositor restarts it, and your windows stay open.
- A **secure lock screen** (`ext-session-lock-v1`): if the locker dies, the screen stays
  black and locked, and the locker is restarted.
- **GPU resets are handled.** If the Vulkan device is lost, the compositor stops
  touching the GPU and exits cleanly, so the supervisor can start a new one
  ([policy](vulkan-device-loss.md)).
- **User Account Control.** When an app needs administrator rights, the whole desktop
  dims and blurs and one dialog asks: who is asking, for what, and the password. Nothing
  else takes the keyboard until you answer; requests queue up instead of being refused
  ([design](polkit-agent.md)).
- **Integrates with desktop services**: GTK for generic application portals,
  wlr for screen capture, and the KDE wallet (so Chrome,
  Brave and VS Code keep their logins), color schemes and icon themes.

## Under the surface

A modern shell is only the surface. Vela also controls the path underneath it, from the
moment an app commits a Wayland surface to the moment the pixel lights up on your monitor.

```
input → state → predicted frame → render as late as possible → presentation feedback → correct the model
```

- **Its own scene graph and Vulkan renderer** on top of wlroots, with no `wlr_scene` and
  no wlroots renderer. Flattening, occlusion, damage history with buffer age, damage
  expanded for blur, and per-surface pacing are all Vela's.
- **A frame clock that models time.** Each output keeps a grid of real vblanks and
  measures what frames cost (worst case over the last second). It starts rendering as
  late as it safely can and widens its margin after a miss. It even tells a slow frame
  apart from constant latency added by a host compositor.
- **A slow app can't hold the screen back.** A commit whose buffer the app's GPU is
  still drawing waits for its fence (explicit or implicit sync), and until then the
  screen keeps the app's previous frame: the cursor, the animations and every other
  window stay on the vblank. Buffers stay imported only while they're on screen, because
  the kernel would otherwise make every frame wait for each app redrawing one of them.
  Measured with an app whose GPU finishes 150 ms late: 0 missed vblanks, down from 47.
- **Direct scanout done properly.** A single opaque surface covering the output 1:1
  goes straight to the display plane, with dmabuf feedback and explicit-sync timelines.
  The moment that stops being true, Vela falls back to compositing.
- **Fractional scaling it can prove.** Exact preferred scales, pixel-snapped
  positions, and nearest sampling whenever a buffer maps 1:1, verified bit for bit by
  an automated test.

The whole design is written down in [docs/renderer.md](renderer.md).
