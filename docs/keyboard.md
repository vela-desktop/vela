# Keyboard

Design of Vela's keyboard settings: a Keyboard section of its own in
Settings, a layout picker that lists every layout the system has, in the
user's language, with flags and a way to try a layout before adding it,
what the Caps Lock, Compose, Ctrl, Alt and Win keys do, Num Lock at
startup, Caps Lock you can see, custom shortcuts, the Win key alone, and
the terminal that Win+Enter opens. Windows is one reference among KDE
Plasma, GNOME, macOS and COSMIC, never the rule.

> **Status:** Accepted: designed with the maintainer on 21 November 2026,
> no code yet. **Checked:** 21 November 2026, `b236a81`.
>
> **Remaining:** every stage of [§13](#13-stages), then the real-session
> checks of [§15](#15-tests).

## 1. Goals

1. **Every layout, in your language.** The picker lists all the layouts
   the system has, named in the language Vela speaks, and finds them by
   any name a person would type.
2. **Try before you add.** A layout can be seen and typed with before it
   becomes one of yours, without touching the layout in use.
3. **Keys do what you want.** The few key changes most people ask for
   (Caps Lock, Compose, the bottom-left modifiers, the numeric keypad)
   are choices in Settings, not XKB option strings.
4. **Lock keys you can predict.** Num Lock is on or off at login as you
   choose; Caps Lock says when it changes, and password fields say when
   it is on.
5. **Your shortcuts.** A key combination can open an app, run a command or
   open a link or file, and Settings says plainly when a combination takes
   keys away from apps.
6. **Vela's settings only.** What Settings shows is what Vela uses: no
   setting is silently taken from another desktop's files.

## 2. Decisions taken

| Topic | Decision | Elsewhere |
|---|---|---|
| Where | A **Keyboard** section at the top level of Settings; the layouts move there from Time & language. | GNOME: Keyboard panel; KDE: Input Devices > Keyboard; macOS: Keyboard; Windows: Time & language. |
| Catalogue | `evdev.xml`, `evdev.extras.xml` and the user's own XKB rules, grouped by layout, no cap on the list. | KDE and GNOME read `evdev.xml`; KDE offers extras behind a switch. |
| Names | Translated with xkeyboard-config's own translations, searchable in English too. | GNOME and KDE translate the same way. |
| Flags | The layout's country flag, bundled SVGs, generic keyboard icon when there is no single country. | macOS shows flags; KDE optional in its indicator; GNOME, Windows none. |
| Trying | A drawing of the keyboard and a field typed through the candidate layout, before *Add*. | GNOME and KDE show a drawing; macOS a Keyboard Viewer of the active source; none lets you type with a layout you haven't added. |
| Key behavior | Four curated choices; the rest of XKB stays in `vela.conf`. A full option list is deferred to 0.3. | macOS: Modifier Keys dialog; GNOME: Compose and alternate characters; KDE: the full list. |
| Num Lock | Off (today's behavior), on, or as it was at the end of the last session. | KDE: on, off, unchanged; GNOME remembers; Windows: a registry value. |
| Caps Lock | A warning in every Vela password field, and a short on-screen display on each toggle. | macOS: an indicator at the text cursor; Windows: OEM utilities. |
| Custom shortcuts | Their own page: app, command, or link/file; Vela's and reserved combinations refused; combinations apps may use warned about. | GNOME: name, command, shortcut; KDE: app, command, script; COSMIC: command. |
| Vela's own shortcuts | Fixed for now; an editor is deferred to 0.3. | |
| Win key alone | Opens Start unless the user turns that off. | GNOME, KDE and Windows open their launcher; macOS has no such key. |
| Terminal | One lookup for Win+Enter and the updater, following the freedesktop default-terminal proposal, with a choice in Default apps. | xdg-terminal-exec implements the proposal; KDE: kdeglobals; GNOME: its own setting. |
| KDE's files | The compositor stops reading `kxkbrc`; Settings stops naming KDE for the layout. | |

## 3. The keyboard today

- **Settings > Time & language > Keyboard** lists the installed layouts
  (add, remove, move up), the key repeat delay and rate, and a test field.
  With no layout chosen it says *Chosen by localectl or KDE*.
- **The layout picker** reads only `evdev.xml` and shows the first 200 of
  601 entries in English, sorted by name: browsing stops at *French (alt.,
  Latin-9 only)*, and Italian, German, Spanish or Russian show up only when
  searched for. The 44 layouts and 152 variants of `evdev.extras.xml`
  (EurKEY, APL, the International Phonetic Alphabet, more Italian, German
  and French variants) are missing. A click on a row adds the layout at
  once.
- **More than four layouts:** XKB keymaps hold at most four. Settings
  lets you add a fifth; libxkbcommon then ignores it with an error in the
  log, and Win+Space never reaches it.
- **The layout's sources**, in order (`compositor/src/keyboard.c`): the
  `XKB_DEFAULT_*` variables, `vela.conf`, KDE's `kxkbrc` when KDE manages
  the keyboard, systemd-localed's `/etc/X11/xorg.conf.d/00-keyboard.conf`.
  Several layouts add `grp:win_space_toggle` unless `keyboard-options`
  already has a `grp:` option.
- **`keyboard-options`** exists in `vela.conf` but has no control in
  Settings.
- **Lock keys:** each physical keyboard has its own XKB state, so Caps
  Lock pressed on an external keyboard is not on for the laptop's. Every
  keyboard starts with Num Lock off: on Wayland the compositor's XKB state,
  not the firmware, decides it.
- **Caps Lock warnings:** the polkit dialog guesses the state from the
  letters typed (an uppercase letter without Shift); `vela-lock` says
  nothing.
- **Shortcuts** are written in `compositor/src/bindings.c`. Win+Enter
  tries `VELA_TERMINAL`, then `konsole || foot || kitty || alacritty ||
  xterm`; the updater's *Open in a terminal* (`updates/src/report.c`)
  tries `xdg-terminal-exec`, then `VELA_TERMINAL`, then `konsole`.
- **Win alone** always opens Start.

## 4. Settings > Keyboard

A top-level section, between *Time & language* and *Accessibility*, with
the icon `input-keyboard`. Its page links to:

| Page | Contents |
|---|---|
| **Layouts** | The installed layouts (§5.5) and *Add a keyboard* (§5.1–5.4). |
| **Typing** | Key repeat and its test field (today's), the keys (§6), Num Lock at startup (§7), the Caps Lock display (§8.3), the Win key (§10). |
| **Custom shortcuts** | §9. |

*Vela's shortcuts*, the editor of 0.3, will be a fourth page here. Time &
language loses its Keyboard link; the search entries for layouts, repeat
and shortcuts point to the new pages, and the old page name `keyboard`
opens the section, so links from the shell (for example a future layout
indicator) keep working.

## 5. Layouts

### 5.1 The catalogue

`KeyboardLayouts` reads, in order, and merges by layout and variant name:

1. `/usr/share/X11/xkb/rules/evdev.xml`;
2. `/usr/share/X11/xkb/rules/evdev.extras.xml`;
3. the user's rules, `$XDG_CONFIG_HOME/xkb/rules/evdev.xml`, which
   libxkbcommon already loads, so a layout the user wrote there works
   once added.

Each **layout** is a group: its own entry (no variant) first, then its
variants. From each `configItem` it keeps the name, the description, the
`iso3166Id` countries and the `iso639Id` languages. The list is a
virtualized view of about 145 groups: no cap, and browsing reaches every
layout. A layout already installed is marked and can't be added twice.

**Four at most.** With four layouts installed *Add a keyboard* is
disabled and says why (*A keyboard can switch between up to four
layouts*).

### 5.2 Names and search

Descriptions are translated with xkeyboard-config's gettext domain
(`dgettext("xkeyboard-config", …)`), in the language Vela's interface
uses; an untranslated description stays in English. Groups are sorted by
the translated name, with the variants in file order under their layout.

The search box matches, case- and accent-insensitively, any of:

- the translated name and the English one (*Italiana*, *Italian*);
- the code (`it`, `it(us)`);
- the names of the layout's languages and countries, in English and in
  their own language, from `QLocale` (*italiano*, *Italia*, *Deutsch*).

A group is shown when its layout or any of its variants matches, expanded
to the matching variants.

### 5.3 Flags

Each entry shows its country's flag: the first `iso3166Id` of the variant,
else of its layout. Layouts with no single country (Braille, Esperanto,
APL, BQN, IPA, *ancient*, *custom*) and those that list several (`latam`,
`ara`) show the generic `input-keyboard` icon. The flag marks the
layout's country, which is how XKB names layouts; it is not a statement
about the language.

The flags are the SVGs of [flag-icons](https://github.com/lipis/flag-icons)
(MIT), the 4×3 set, in `settings/flags/` with the project's licence and
version, compiled into Settings' resources. Only the countries that
xkeyboard-config names are kept; a country without a file falls back to
the generic icon. The size of the set is measured when it lands, and the
SVGs are optimized if needed.

### 5.4 Try before adding

*Add a keyboard* becomes a two-pane dialog: the list and search on one
side, the chosen entry's preview on the other, and *Add* at the bottom,
enabled once an entry is chosen. A double click on an entry also adds it.

**The drawing.** Settings compiles the entry's keymap with libxkbcommon,
with the key options of §6, and draws the main block of a keyboard: for
each key, the symbols of levels 1 and 2, and of level 3 when the layout
has an AltGr. Holding Shift or AltGr in the dialog brings that level
forward. XKB has no geometry, so the shapes are Vela's own tables of evdev
key codes: ISO 105 keys by default, ANSI for the layouts of countries that
use it (`us`, among others), JIS for `jp` and ABNT2 for `br`; a small
choice in the preview switches the shape.

**The try field.** A text field under the drawing that types *through the
candidate layout*: on each key event it takes the key code (on Wayland,
`QKeyEvent::nativeScanCode()` is the XKB key code), feeds its own
`xkb_state` and inserts `xkb_state_key_get_utf8()`, with dead keys and
Compose through `xkb_compose` for the locale. Backspace, Delete, the
arrows, Home, End and the Ctrl shortcuts edit the field as usual. The key
pressed lights up in the drawing. The layout in use by the rest of the
desktop never changes, so nothing has to be put back if the dialog is
closed.

The installed layouts (§5.5) open the same preview from their *Preview*
button.

### 5.5 The installed layouts

As today: the list in Win+Space order, the first one the default, with
*Move up* and *Remove*, and the flag and translated name of each. With no
layout chosen the card says *From the system* and names the layout
systemd-localed configured, read from the same file the compositor reads;
with none there either, *English (US)*.

### 5.6 No KDE settings

The compositor stops reading `kxkbrc`. The layout comes from
`XKB_DEFAULT_*`, then `vela.conf`, then systemd-localed, then `us`, so
what Settings shows is what Vela uses. A user who set layouts in Plasma
and starts Vela for the first time gets the system's layout until they
add theirs in Settings. In a nested session the `XKB_DEFAULT_*` variables
still choose the layout.

## 6. Key behavior

Settings > Keyboard > Typing, group *Keys*:

| Control | Choices | XKB option |
|---|---|---|
| **Caps Lock key** | Caps Lock (default) · Ctrl · Esc · Backspace · Off | — · `ctrl:nocaps` · `caps:escape` · `caps:backspace` · `caps:none` |
| **Compose key** | None (default) · Right Alt · Right Win · Menu · Right Ctrl · Caps Lock · Scroll Lock | — · `compose:ralt` · `compose:rwin` · `compose:menu` · `compose:rctrl` · `compose:caps` · `compose:sclk` |
| **Bottom-left keys** | Standard (default) · Swap Ctrl and Alt · Swap Alt and Win (Mac keyboards) | — · `ctrl:swap_lalt_lctl` · `altwin:swap_lalt_lwin` |
| **Numeric keypad** | *Always type digits* switch, off by default | `numpad:mac` |

Rules:

- **Compose on Caps Lock** takes the key: the Caps Lock control then
  shows *Used as Compose* and is disabled.
- **Compose on Right Alt** with a layout that types characters with AltGr
  (Italian, German, French…) says, under the control, that those
  characters need another AltGr key.
- **The swaps are left-side only**, so the right Alt stays AltGr. They are
  one choice because the two options rewrite the same keys.
- **PrtSc is not offered** for Compose: it opens the snipping tool.
- **Num Lock at startup** (§7) is disabled while the keypad always types
  digits, with a note.
- **Other options are kept.** Settings owns only the options in the table
  and rewrites only those in `keyboard-options`; any other option a user
  wrote there stays. When `keyboard-options` already holds an option that
  changes the same keys (for example `ctrl:swapcaps`), the control shows
  *Custom (vela.conf)* until the user picks a choice, which replaces it.
- The forced `grp:win_space_toggle` (§3) is unchanged.

The changes apply at once through `reload-config`, to every keyboard, and
the try field of §5.4 uses them too.

## 7. Num Lock at startup

`numlock=off|on|last` in `vela.conf`, Settings > Keyboard > Typing,
*Num Lock when Vela starts*: **Off** (default, as today), **On**, **As it
was** (the state at the end of the last session).

- Every keyboard starts with Num Lock off on Wayland whatever the
  firmware did, so *leave it unchanged* would mean *off*: it is not a
  separate choice.
- *As it was* keeps the state in `$XDG_STATE_HOME/vela/numlock` (`on` or
  `off`), written when the state changes, at most once a second.
- The state is applied when the first physical keyboard appears. Later
  keyboards take the current state (§8.1), so plugging a keyboard in
  doesn't change it.
- Virtual keyboards (tests, remote desktop) send their own modifiers and
  are left alone.
- Off is the default because some older laptops put a keypad under the
  letter keys, where Num Lock on turns *uiop* into digits.

## 8. Caps Lock

### 8.1 One state for every keyboard

Caps Lock and Num Lock become the seat's, as in GNOME and KDE: when a
key on one physical keyboard changes the locked modifiers, the
compositor gives the others the same locked mask, keeping their pressed
and latched modifiers and their layout. It fits the sticky keys' handling
(`restoring` already guards re-entrant modifier updates). The layout
itself stays per keyboard, as today (see §14).

### 8.2 The warning in password fields

*Caps Lock is on* under the password field, in the caution color, while
the field has the focus and Caps Lock is on, from the real state:

- **vela-lock** already tracks `wl_keyboard.modifiers`; it tests the
  keymap's `Lock` modifier in the locked mask and draws the line under
  its field.
- **The polkit dialog** stops guessing from letters: it asks Qt's
  `wl_seat` (`QNativeInterface::QWaylandApplication::seat()`) for a second
  `wl_keyboard`, whose `keymap` and `modifiers` events give the same
  state.

### 8.3 The on-screen display

When the shared Lock state changes because of a key press, the compositor
sends the shell `caps-lock on` or `caps-lock off`. The shell shows a
small translucent pill, at the bottom in the middle like the volume
display (it takes the volume display's place if both are up), with the
Caps Lock icon and *Caps Lock on* or *Caps Lock off*. It ignores the
pointer, is announced to screen readers, and fades after 1.5 seconds
(scaled with Vela's animation duration).

- It follows the **Lock state, not the key**: with Caps Lock mapped to
  Ctrl, Esc, Backspace, Compose or nothing, Lock never changes and no
  display appears; with a key mapped *to* Caps Lock, it does.
- Restoring state (§7) and syncing keyboards (§8.1) never show it.
- **On the lock screen** the shell's layers are hidden by design
  (`compositor/src/lock.c`), so `vela-lock` draws the same pill on its
  own surface.
- A switch, *Show Caps Lock on screen* (`caps-lock-osd=yes`), turns it
  off.

## 9. Custom shortcuts

### 9.1 The page

Settings > Keyboard > Custom shortcuts lists each shortcut with its
combination, what it does (the app's icon and name, the command, or the
link or file) and *Edit* and *Remove*. *Add a shortcut* opens a dialog:

1. **What it does:** *Open an app* (a searchable list of installed apps),
   *Run a command* (a text field), *Open a link or file* (a text field,
   with *Browse* for a file).
2. **The combination:** *Record*, then the keys. While recording,
   Settings holds a `zwp_keyboard_shortcuts_inhibitor_v1` on its window,
   so Win and Vela's combinations reach it instead of acting.
3. **Save**, enabled once both are set and the combination is allowed.

### 9.2 What can be used

Refused, with the reason under the combination:

- **No modifier:** a plain key or Shift with a key would stop typing it.
  F13–F24 and the special keys Vela doesn't use (Calculator, Mail,
  Favorites…) are allowed alone.
- **Vela's own:** every combination of `docs/shortcuts.md` in the
  current mode (the Win+number keys only in the mode that uses them).
  They come from one table in `common/` that the compositor's bindings
  and Settings both read; the 0.3 editor grows it into the binding
  table.
- **Reserved:** Ctrl+Alt+F1–F12, Win+L, Alt+Shift+Esc, the media and
  volume keys.
- **Already a custom shortcut:** Settings offers to replace it.

**Warned about**, with a prominent caution bar that stays while the
combination is shown, and a confirmation on *Save*: any combination
without Win. Ctrl, Alt and Ctrl+Shift combinations are what apps use
(Ctrl+Alt+T, Ctrl+Shift+Esc…), and a custom shortcut takes them from
every app, without exception.

### 9.3 The file

`$XDG_CONFIG_HOME/vela/shortcuts.conf`, written by Settings, one line per
shortcut, the combination with its modifiers in a fixed order and the key
as its XKB keysym name:

```ini
# Vela's custom shortcuts (Settings > Keyboard > Custom shortcuts)
Ctrl+Alt+t=app org.kde.konsole.desktop
Super+Shift+k=command kcalc
Super+F1=open https://docs.example.org/
```

The kinds are `app` (a desktop file ID), `command` (a shell command line)
and `open` (a URL or an absolute path). A line that doesn't parse is
skipped with a warning in the log. The file is the user's, like
`vela.conf`: whoever can write it can already run anything as the user.

### 9.4 Running them

The compositor reads the file at startup and on `reload-config`, and
matches it in `vela_bindings_handle` after the lock, the polkit dialog and
`keyboard-shortcuts-inhibit` (so a VM or remote desktop keeps them), the
same way it matches its own: the modifiers held and the key's keysym
without Shift. It sends the shell `shortcut <kind> <target>`. The shell
runs it as Start would: an app with an activation token, so its window
comes forward under the focus rules of
[#264](https://github.com/vela-desktop/vela/pull/264); a command with
`sh -c`, detached; a link or file with its default app.

## 10. The Win key alone

`super-opens-start=yes|no`, Settings > Keyboard > Typing, *Open Start
with the Win key*, on by default. Off, a press and release of Win alone
does nothing; Win combinations, the taskbar numbers while Win is held and
the Start button are unchanged. In a nested session Alt+S stays.

## 11. The terminal

One lookup in `common/` (C, for the compositor and the updater; Settings
lists from it), used by Win+Enter and by the updater's terminal:

1. **The user's choice:** the first entry of
   `$XDG_CONFIG_HOME/vela-xdg-terminals.list`, which Settings writes. It
   is the desktop-specific list of the freedesktop default-terminal
   proposal (Vela's `XDG_CURRENT_DESKTOP` starts with `Vela`), so a Plasma
   or GNOME session on the same account keeps its own choice.
2. **`VELA_TERMINAL`**, for compatibility.
3. **`xdg-terminal-exec`**, when installed: it applies the rest of the
   proposal, its lists and its exclusions.
4. **The installed terminals:** desktop files in the applications
   directories with `Categories=TerminalEmulator`, not hidden and allowed
   in Vela by `OnlyShowIn`/`NotShowIn`, in desktop-ID order, without
   drop-down terminals (Yakuake, Guake). Kwi, Ghostty, WezTerm and the
   others are found this way, with no name written in Vela.
5. The fixed list of today, as a last resort when there are no desktop
   files.

A command to run (the updater's case) is passed with the desktop file's
`X-TerminalArgExec`, else `-e`.

**Settings > Apps > Default apps** gets a *Terminal* row among the common
uses: *Automatic* (steps 2–5) and the terminals of step 4; a choice
writes or clears the list of step 1.

## 12. Configuration

New and changed `vela.conf` keys, documented in `docs/configuration.md`:

| Key | Default | Meaning |
|---|---|---|
| `keyboard-options` | empty | XKB options. Settings owns those of §6 and keeps the others. |
| `numlock` | `off` | `off`, `on` or `last`: Num Lock when Vela starts (§7). |
| `caps-lock-osd` | `yes` | The Caps Lock display (§8.3). |
| `super-opens-start` | `yes` | Win alone opens Start (§10). |

Other files: `$XDG_STATE_HOME/vela/numlock` (§7),
`$XDG_CONFIG_HOME/vela/shortcuts.conf` (§9.3),
`$XDG_CONFIG_HOME/vela-xdg-terminals.list` (§11).

## 13. Stages

One pull request, one commit per stage; each commit updates the user
documentation it changes (`docs/shortcuts.md`, `docs/configuration.md`,
`docs/features.md`, `CHANGELOG.md`).

| Stage | Contents |
|---|---|
| KP1 | The Keyboard section (§4), *From the system* (§5.5), no `kxkbrc` (§5.6). |
| KP2 | The catalogue, names, search and the four-layout limit (§5.1, §5.2). |
| KP3 | Flags (§5.3). |
| KP4 | Try before adding: the drawing, the try field, *Preview* (§5.4). |
| KP5 | Key behavior (§6). |
| KP6 | One lock state for every keyboard and Num Lock at startup (§7, §8.1). |
| KP7 | The Caps Lock warning in vela-lock and the polkit dialog (§8.2). |
| KP8 | The Caps Lock display, in the shell and vela-lock (§8.3). |
| KP9 | Custom shortcuts: file, compositor, shell, Settings page (§9). |
| KP10 | The Win key alone (§10). |
| KP11 | The terminal lookup and Default apps > Terminal (§11). |

## 14. Not in scope

Proposed for 0.3 in the [roadmap](roadmap.md):

- an editor for Vela's own shortcuts (rebind, turn off, conflicts);
- a layout indicator on the taskbar and an on-screen layout switcher;
- a choice of the layout-switch combination, and a layout per window or
  per app;
- one layout for every keyboard of the seat, as §8.1 does for the locks;
- reordering layouts by dragging, and custom layout labels;
- a full list of XKB options (*Advanced*);
- the special keys (Calculator, Mail, Search…) bound to Vela's actions;
- accents on a held key and an emoji picker, with the input methods;
- keyboard backlight, with its keys and display;
- slow keys, bounce keys and mouse keys, next to sticky keys.

## 15. Tests

**Unit tests**

- Catalogue: fixture rules files (base, extras, user) merge, group, keep
  countries and languages; the four-layout limit.
- Names and search: translated and English names, codes, languages,
  countries, accents; a group expands to its matching variants.
- Flags: the variant's country, then the layout's; none or several →
  the generic icon.
- Try field: a keymap compiled for `it` turns the key codes of *ò*, *à*
  and AltGr+*è* into those characters; a dead key and a Compose sequence
  for `us(intl)`.
- Options: each control writes its option, keeps unknown ones, shows
  *Custom* for a conflicting one; Compose on Caps disables the Caps
  control.
- Compositor: the Num Lock mask for each mode and saved state; lock sync
  between two keyboards keeps the others' pressed modifiers and layout;
  the shortcuts file parser and the combination normalization; the
  conflict table against `bindings.c`.
- Terminal lookup: user list, `VELA_TERMINAL`, `xdg-terminal-exec`,
  desktop files (hidden, `NotShowIn`, drop-down), the fixed list,
  `X-TerminalArgExec` (extends `updates/tests/report_test.cpp`).

**Headless functional tests** (`scripts/run-headless.sh`, `vela-input`)

- Settings: the Keyboard section and its pages; adding a layout from the
  preview; *From the system*.
- A custom shortcut of each kind runs (a command that writes a file, an
  app, a URL to a fake default app); a Vela combination is refused; a
  Ctrl+Alt one shows the warning; recording catches Win combinations.
- The Caps Lock display on each toggle, not with Caps Lock as Ctrl, not
  with `caps-lock-osd=no`.
- Win alone with `super-opens-start=no`.
- Win+Enter with a fake terminal desktop file and with a user choice.

**Real session** (by the maintainer)

- Num Lock at login in each mode; two physical keyboards share Caps and
  Num Lock and their LEDs.
- The Caps Lock display and warning on the lock screen and in the polkit
  dialog.
- Compose and the swaps on a physical keyboard; a Mac keyboard if
  available.
