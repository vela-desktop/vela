# System appearance

Settings → Personalization → Themes → System appearance chooses what Vela
shares with KDE and Qt apps: the system font, the icon theme, and the cursor
theme and size. This document says where those choices are stored, how Vela
writes them without KDE, and how every part of the desktop picks them up at
once.

> **Status:** Implemented. **Checked:** 28 November 2026.
>
> **Remaining:** a run in a real session (§8), and the limits in §7.

Based on the work of Madvith-d in
[#312](https://github.com/vela-desktop/vela/pull/312).

## 1. Goals

1. **Shared.** A font or theme chosen in Vela is the one KDE and Qt apps
   use, and a choice made in KDE's settings shows in Vela. There is no second
   copy that could disagree.
2. **KDE optional.** It works the same with or without Plasma, KDE's
   platform plugin or `kwriteconfig6`.
3. **At once.** The title bars, the pointer, the shell, Vela's apps and
   running KDE/Qt apps change without a restart.
4. **Honest.** Settings offers only what is installed. When a write fails
   or KDE marks the setting immutable, the file stays as it was, the choice
   goes back to what the file says and Settings names the file it could not
   write.

## 2. Where the choices live

KDE's own files. The compositor already read them for the title bars
(`compositor/src/text.c`, `icons.c`), and KDE's platform plugin reads them
for every Qt app.

| Choice | File | Keys |
| --- | --- | --- |
| System font | `kdeglobals` | `[General]` `font`, `menuFont`, `toolBarFont`, `smallestReadableFont`; `[WM]` `activeFont` (title bars) |
| Icon theme | `kdeglobals` | `[Icons]` `Theme` |
| Cursor theme | `kcminputrc` | `[Mouse]` `cursorTheme` |
| Cursor size | `kcminputrc` | `[Mouse]` `cursorSize` (logical pixels, 8–256) |

The font keys are the ones Plasma's font settings write, except the fixed-width
font. Each key keeps its own size and weight; only the family changes.

The lists hold only what is installed. Fonts come from fontconfig. Icon and
cursor themes come from `~/.icons` and from `icons/` in the XDG data
directories. An icon theme has icon directories (`Directories=` in its
`index.theme`). A cursor theme has `cursors/default` or `cursors/left_ptr`.
So `breeze_cursors` appears only in the cursor list.

The cursor size is a field with − and + beside it, from 8 to 256, that always
lands on a size the cursor theme is drawn at. Those sizes are the nominal
sizes in the table of contents of the theme's arrow (its Xcursor file), or of
the theme it inherits (`default` naming Adwaita). − and + go to the previous
or next one: from 24 with Capitaine, + gives 36. A typed size goes to the
nearest; halfway between two, to the smaller, as wlroots chooses. The reason:
wlroots shows the theme's drawing nearest to the size asked, unchanged, so a
size between two would show one of them anyway. A size written elsewhere (by
KDE) shows as the one the pointer really has. When the theme's sizes can't be
read, every size from 8 to 256 is accepted. A question mark beside the field
opens this explanation, with the theme's sizes and what the display scale
does to them (§6).

## 3. Writing without KDE

`common/src/kdeconfig.h` (`vela-kdeconfig`) reads and writes these files the
way KConfig does:

- **Line by line.** A write rewrites the key's lines in place. A missing key
  goes at the end of its group, and a missing group at the end of the file.
  Every other line stays byte for byte: comments, other groups, localized
  keys (`key[it]`), flags.
- **KConfig's escapes.** Values are read and written with `\s`, `\t`, `\n`,
  `\\` and `\xNN`. Commas stay as they are.
- **Atomic, under KConfig's lock.** All the keys of one choice go in a
  single replacement of the file (`QSaveFile`), taken under `<file>.lock`, the
  lock KConfig uses. A KDE app saving the same file can't interleave with
  Vela. A failure leaves the old file whole. A symbolic link is followed and
  the file's mode is kept.
- **Immutable is final.** `[$i]` on the file, the group or the key makes the
  write fail. That is the administrator's lock.

QSettings is not a KConfig reader. It maps `[General]` to its own top-level
keys, reads `Noto Sans,10,-1,5,400,…` as a list (an empty string) and writes a
value with commas in quotes that KDE then keeps as part of the value.
`kwriteconfig6` would be correct but would make KDE a dependency, with a
second code path for systems without it.

## 4. Telling the rest of the desktop

After a successful write, Settings sends:

| To | Message | For |
| --- | --- | --- |
| The compositor | `reload-font`, `reload-icons`, `reload-cursor` | title bars, pointer (§6) |
| KConfig watchers | `org.kde.kconfig.notify.ConfigChanged` on `/kdeglobals` or `/kcminputrc`, the changed keys | what `kwriteconfig6 --notify` sends |
| KDE's platform plugin | `org.kde.KDEPlatformTheme.refreshFonts` | running Qt apps reload their fonts |
| KDE's platform plugin | `org.kde.KGlobalSettings.notifyChange` (1 font, 4 icons, 5 cursor) | running Qt apps reload their icon theme |
| KIconLoader | `org.kde.KIconLoader.iconChanged` | KDE apps empty their icon cache |

Vela's own apps (the shell, Settings, Files) watch `kdeglobals`. When the icon
theme changes, `Style.iconRevision` goes up. Every icon address carries it
(`image://icon/v<n>/l/<name>`, `Theme.fileIcons`, `Theme.versionedIcon()`),
so QML asks for each icon again.

## 5. The icon theme and the light or dark mode

`[Icons] Theme` is the theme as chosen. Vela's apps show its light or dark
variant to match the mode (`breeze` or `breeze-dark`, `applyIconTheme()` in
`shell/src/iconprovider.h`). The variant is never written back. Before this
change, switching the mode wrote the variant into `kdeglobals` and replaced
the user's choice. KDE apps follow the mode by themselves: KIconLoader
recolors the theme's monochrome icons with the app's palette.

Without a choice, the theme is the platform's. If the platform has none
(`hicolor`), it is `VELA_ICON_THEME`, else Breeze Dark. `hicolor` that was
actually chosen stays `hicolor`.

## 6. The cursor theme in the compositor

At startup, and on `reload-cursor`, the compositor reads `[Mouse]
cursorTheme` and `cursorSize` from `kcminputrc`. Without them, or with a name
that is not a single directory name or a size out of 8–256, it falls back to
`XCURSOR_THEME` and `XCURSOR_SIZE`, else the `default` theme at 24. The size is
logical: wlroots loads the theme again at each output's scale, so a 24 px
cursor is 30 physical pixels at 125%. On `reload-cursor` it:

1. creates a manager for the theme and size;
2. shows the current cursor again from it, if the pointer is showing one of
   the theme's cursors (Vela's own, or one an app asked for by name with
   cursor-shape-v1);
3. gives Xwayland its new default cursor;
4. destroys the old manager.

`wlr_cursor` holds a manager, and an animated cursor's timer holds its
images, only while one of that manager's cursors is shown. Step 2 guarantees
that nothing holds the old one in step 4. All the pointer image changes go
through `vela_input_set_cursor_name()` and `vela_input_set_cursor_surface()`,
so the compositor always knows which kind is shown. An app's own cursor
surface stays as it is, and so does a hidden pointer: an app hides the pointer
by setting no surface, and a new theme must not bring it back.

## 7. Limits

- **GTK apps** read GNOME's settings (`org.gnome.desktop.interface`), not
  KDE's files. Writing those too is a separate choice.
- **Font size** is not offered.
- **Apps that draw their own cursor** from `XCURSOR_THEME` and
  `XCURSOR_SIZE` (rare on Wayland; some X11 apps) keep what they started
  with.

## 8. Tests

- `kdeconfig`: the reader and writer against Plasma's own files. It checks
  escapes, locales, subgroups, `[$d]`, the immutable marks, a busy lock, an
  unreadable file, a symbolic link and the file's mode, what `kreadconfig6`
  reads back (when installed), and the D-Bus messages on a private bus.
- `settings-preferences`: the font without `kwriteconfig6` (every key, sizes
  and weights kept, no quotes, the real `[General]`), the icon and cursor
  lists (a cursor-only theme in the right list, `~/.icons`), the chosen
  theme written rather than its dark variant, an immutable `kcminputrc`
  reported, and the cursor sizes (from an Xcursor table of contents, through
  `Inherits`; the steps of − and +, a typed size snapped to the nearest, only
  the theme's sizes written; any size when they can't be read).
- `settings-controls` (`tst_cursorsizefield`): the field's − and +, a typed
  size and where it lands, the ends, a size set elsewhere, and the question
  mark's popup.
- `shell-icon-provider`: the configured theme read again at every call, an
  explicit `hicolor`, `~/.icons` in the search paths, and a test
  `kdeglobals` so the host's theme can't interfere.
- `functional-system_appearance` (GPU): read on screen. It checks the cursor
  from `kcminputrc` at startup (theme and size), a new theme and a new size
  without moving, a name outside the cursor directories refused,
  over a window border after an app's own cursor, an app's cursor and a
  hidden one left alone, and an animated cursor replaced while outputs come
  and go. It also checks the title bars' icons following a new theme, an
  inactive window's icon staying dimmed, and a title moving after an icon
  that appears. `vela-pattern --cursor surface|hidden` plays the apps.

Each test was checked to fail with its fix removed. The cursor tests also run
against an AddressSanitizer build.

In a real session: choose a font, an icon theme and a cursor theme with
Dolphin and Kate open, then check them after a restart of the session.
