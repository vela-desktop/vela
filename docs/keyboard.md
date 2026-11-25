# Keyboard

Design of Vela's keyboard settings: a Keyboard section of its own in
Settings, a layout picker that lists every layout the system has, in the
user's language, with a way to try a layout before adding it,
what the Caps Lock, Compose, Ctrl, Alt and Win keys do, Num Lock at
startup, Caps Lock you can see, custom shortcuts, the Win key alone, and
the terminal that Win+Enter opens. Windows is one reference among KDE
Plasma, GNOME, macOS and COSMIC, never the rule.

> **Status:** Accepted: designed with the maintainer on 21 November 2026;
> KP1–KP11 implemented, merged in PR #311 on 22 November 2026.
> **Checked:** 21 November 2026, `bc86a1e`.
>
> **Remaining:** the real-session checks of [§15](#15-tests).

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
| Catalogue | libxkbregistry, with the extras and the user's own rules, grouped by layout, no cap on the list. | GNOME and KDE read the same registry; KDE offers extras behind a switch. |
| Names | Translated with xkeyboard-config's own translations, searchable in English too. | GNOME and KDE translate the same way. |
| Flags | None: every layout has the generic keyboard icon. | macOS shows flags; KDE optional in its indicator; GNOME, Windows none. |
| Trying | A drawing of the keyboard and a field typed through the candidate layout, before *Add*. | GNOME and KDE show a drawing; macOS a Keyboard Viewer of the active source; none lets you type with a layout you haven't added. |
| Key behavior | Four curated choices; the rest of XKB stays in `vela.conf`. A full option list is deferred to 0.3. | macOS: Modifier Keys dialog; GNOME: Compose and alternate characters; KDE: the full list. |
| Num Lock | Off (today's behavior), on, or as it was at the end of the last session. | KDE: on, off, unchanged; GNOME remembers; Windows: a registry value. |
| Caps Lock | A warning in every Vela password field, and a short on-screen display on each toggle. | macOS: an indicator at the text cursor; Windows: OEM utilities. |
| Custom shortcuts | Their own page: app, command, or link/file; Vela's and reserved combinations refused; combinations apps may use warned about. | GNOME: name, command, shortcut; KDE: app, command, script; COSMIC: command. |
| Matching | One binding table and one matcher for Vela's shortcuts and the custom ones, shared with Settings' conflict check; exact modifiers; letters by the symbol on the key (a Latin layout's for non-Latin ones), the digit row and other keys by position. | GNOME and KDE: symbols, with a Latin fallback. |
| Vela's own shortcuts | Fixed for now, moved into the table; an editor is deferred to 0.3. | |
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
- **More than four layouts:** the compositor compiles its keymap with
  `xkb_keymap_new_from_names()` and sends it to apps in the `xkb_v1`
  format of `wl_keyboard`, which holds at most four layouts. Settings
  lets you add a fifth; libxkbcommon then ignores it with an error in the
  log, and Win+Space never reaches it. libxkbcommon 1.11 raised the limit
  to 32 in its v2 format, but clients, Qt among them, parse v1, and
  libxkbcommon itself recommends v1 for keymaps sent to them.
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
- **Shortcuts** are written in `compositor/src/bindings.c`, as `if`s on
  the keysym of the current layout. Most check only that Win is held, so
  extra modifiers still match: Win+Shift+E opens Files and Win+Shift+L
  locks, though `docs/shortcuts.md` lists neither. With a non-Latin
  layout active (Russian, Greek) the letter shortcuts don't fire. The
  Win+number keys alone match the key's position (`super_numbers.c`).
  Win+Enter
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

`KeyboardLayouts` asks **libxkbregistry**, the part of libxkbcommon made
for configuration tools, instead of parsing XML itself:
`rxkb_context_new(RXKB_CONTEXT_LOAD_EXOTIC_RULES)`, then
`rxkb_context_include_path_append_default()` and
`rxkb_context_parse(ctx, "evdev")`. The default include path is the one
libxkbcommon compiles keymaps from (`$XDG_CONFIG_HOME/xkb`, `~/.xkb`,
`/etc/xkb`, then the system's), so the list holds exactly what the
compositor can load: the `evdev.xml` layouts, the extras of
`evdev.extras.xml`, and a layout the user wrote in their own rules. The
registry also gives the option groups and names that §6 uses.

Each **layout** is a group: its own entry (no variant) first, then its
variants. For each entry the registry gives the name, the description,
the short label, the `iso3166` countries and the `iso639` languages;
Settings keeps them and does the grouping, translation and search
itself. The list is a virtualized view of about 100 groups: no cap, and
browsing reaches every layout. A layout already installed is marked and
can't be added twice.

**Four at most.** With four layouts installed *Add a keyboard* is
disabled and says why (*Vela can switch between up to four layouts*).
The limit is the integration's, not XKB's (§3): it can rise when the
keymaps Vela sends to apps can hold more.

### 5.2 Names and search

Descriptions are translated with xkeyboard-config's own catalog, in the
language Vela's interface uses: Settings reads
`/usr/share/locale/<language>/LC_MESSAGES/xkeyboard-config-2.mo` (or
`xkeyboard-config.mo`, the domain's older name) itself, so the process
locale doesn't decide it, and reads it again when the language changes.
An untranslated description stays in English. Groups are sorted by
the translated name, with the variants in file order under their layout.

The search box matches, case- and accent-insensitively, any of:

- the translated name and the English one (*Italiana*, *Italian*);
- the code (`it`, `it(us)`);
- the names of the layout's languages and countries, in English and in
  their own language, from `QLocale` (*italiano*, *Italia*, *Deutsch*).
  For a language or country Qt has no data for (Gothic, Ugaritic), only
  the English name: `QLocale` would answer with the default locale's.

A group is shown when its layout or any of its variants matches, expanded
to the matching variants.

### 5.3 No flags

Layouts show the generic `input-keyboard` icon, not a flag. A flag marks a
country, while a layout serves a language: the English layout isn't the
United States', Arabic, Latin American Spanish, Esperanto and Braille
have no single country, and which flag a layout gets is a political
choice Vela has no reason to make. Flags would also have bundled 129 SVGs
(1.4 MB, 5,800 lines) for a picture the name already says. They were
implemented as KP3 and dropped in the review of PR #311.

### 5.4 Try before adding

*Add a keyboard* shows the list and the search on top and the chosen
entry's preview below, so it fits a narrow window, with *Add* and
*Cancel* at the bottom: *Add* is enabled once an entry not yet installed
is chosen. A click on an entry tries it; a double click adds it.
`KeyboardPreview` (`settings/src/keyboardpreview.h`) does the work.

**The drawing.** Settings compiles the entry's keymap with libxkbcommon
(with the key options of §6 once they exist) and draws the main block of
a keyboard, 15 keys wide and 5 rows high. On each key, the characters of
its levels as printed keys show them: level 1 bottom left, Shift top
left (a letter's capital alone), and, when the layout types anything on
AltGr, AltGr bottom right and Shift+AltGr top right, in the accent
color. A dead key shows the mark it adds; a modifier or other named key
shows its name (*Shift*, *AltGr*, *Compose*), so an option that moves
keys shows on the drawing. The level the key would type now is drawn
full and the others dimmed, and the keys held in the try field are lit:
holding Shift or AltGr there brings that level forward. XKB has no
geometry, so the shapes are Vela's own tables of XKB key names: ISO 105
keys by default, ANSI for the layouts of countries that mostly use it
(`us`, `cn`, `kr`, `tw`, `th`, `il`, `ph`, `id`, `vn`, `my`), JIS for
`jp` and ABNT2 for `br`; a choice beside the try field switches the
shape. The tall ISO Enter is one L-shaped outline.

**The try field.** A text box under the drawing that types *through the
candidate layout*: on each key event it takes the key code (on Wayland,
`QKeyEvent::nativeScanCode()` is the XKB key code), feeds its own
`xkb_state` and inserts `xkb_state_key_get_utf8()`, with dead keys and
Compose through `xkb_compose`, with the compose table of the user's
locale (English's when the locale is C). The layout in use by the rest
of the desktop never changes, so nothing has to be put back when the
dialog closes.

So that no character is typed twice, the field is read-only and owns its
key events: its handler accepts every press and release, so Qt never
inserts the text of the layout in use, and the field inserts only what
the candidate keymap gives. Being read-only, it doesn't accept an input
method either, and it draws its own blinking cursor, which Qt leaves out
of read-only fields. Its `xkb_state` follows the presses and releases,
modifiers included; an auto-repeated press types the key's text again
without pressing it twice, and the repeat of a key pressed before the
field had the focus types nothing. Backspace, Delete, the arrows, Home,
End and Ctrl+A, C, X and V are recognized by the *candidate's* keysym,
so Ctrl+A is where the candidate puts the A, and edit the field; other
Ctrl, Alt and Win combinations type nothing. Tab, Enter and Esc go on to
the dialog: Tab moves the focus, Enter adds, Esc cancels. When the field
loses the focus its state is reset, so a modifier held while leaving it
is not stuck.

The installed layouts (§5.5) open the same preview, alone and with
*Close*, from their *Preview* button.

### 5.5 The installed layouts

As today: the list in Win+Space order, the first one the default, with
*Move up* and *Remove*, and the translated name of each.

The card on top names where the layout in use comes from, following the
compositor's order (§5.6), so Settings never shows a layout Vela isn't
using:

- **`XKB_DEFAULT_*` set** (Settings inherits the session's environment):
  *Set by environment variables*, with the layout they choose. The list
  below stays editable, with a note that the variables win until they are
  removed.
- **Layouts chosen in Settings:** the list, as above.
- **None chosen:** *From the system*, with the layout systemd-localed
  configured, read from the same file the compositor reads.
- **None there either:** *English (US)*.

The compositor logs the same source on every reload (it does already),
and `vela-report` includes that line.

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
- **Other options are kept.** Each control owns one kind of option
  (`settings/src/keyboardoptions.h`): the Caps Lock key's (`caps:*`,
  `ctrl:nocaps`, `ctrl:swapcaps`…), Compose's (`compose:*`), the
  bottom-left keys' (`ctrl:swap_*`, `altwin:swap_*`…) and the keypad's
  (`numpad:*`). A choice replaces its kind's options and keeps every other
  option. When an option of its kind is one the control doesn't offer, or
  there are several (for example `ctrl:swapcaps`), the control shows
  *Custom (vela.conf)* until the user picks a choice, which replaces it.
- **Whose options.** The compositor uses vela.conf's `keyboard-options`
  whenever the key is there, even empty, and systemd-localed's options
  only while it isn't, whichever source the layout comes from
  (`compositor/src/keymap_options.c`). Settings shows the system's options
  until the first choice, then writes the whole list, the system's other
  options included, so nothing is lost when Settings takes over.
- **Win+Space.** With several layouts and no layout-switch option
  (`grp:`), `grp:win_space_toggle` is added, now for the system's layouts
  too.

The changes apply at once through `reload-config`, to every keyboard, and
the drawing and try field of §5.4 use them too.

## 7. Num Lock at startup

`numlock=off|on|last` in `vela.conf`, Settings > Keyboard > Typing,
*Num Lock when Vela starts*: **Off** (default, as today), **On**, **As it
was** (the state at the end of the last session). The logic is
`compositor/src/lock_keys.c`; `keyboard.c` applies it.

- Every keyboard starts with Num Lock off on Wayland whatever the
  firmware did, so *leave it unchanged* would mean *off*: it is not a
  separate choice.
- *As it was* keeps the state in `$XDG_STATE_HOME/vela/numlock` (`on` or
  `off`). It is written atomically (a temporary file, `fsync`, then
  `rename`), one second after the last change so that a burst of presses
  writes once, and again, at once, when the compositor exits normally
  (logout, Alt+Shift+Esc, `SIGTERM`). After a crash it holds the state
  of at most one second earlier. A missing, unreadable or damaged file
  (anything but exactly `on`) means *off*.
- The state is applied when the first physical keyboard appears. Later
  keyboards take the current state (§8.1), so plugging a keyboard in
  doesn't change it.
- Virtual keyboards (tests, remote desktop) send their own modifiers and
  are left alone.
- Off is the default because some older laptops put a keypad under the
  letter keys, where Num Lock on turns *uiop* into digits.
- While the keypad always types digits (§6), the choice is disabled,
  with a note: Num Lock changes nothing then.

## 8. Caps Lock

### 8.1 One state for every keyboard

Caps Lock and Num Lock become the seat's, as in GNOME and KDE. The
compositor keeps **two booleans**, `caps` and `num`, not a modifier mask:

- **Reading.** In `handle_modifiers()` of a physical keyboard, the state
  is read *by name* from that keyboard's keymap: `XKB_MOD_NAME_CAPS` and
  `XKB_VMOD_NAME_NUM`, through `xkb_keymap_mod_get_index()` and
  `xkb_state_mod_index_is_active(…, XKB_STATE_MODS_LOCKED)`. Two
  keyboards may have different keymaps, so their indices are never
  compared or copied.
- **Propagating.** When a boolean changes, every other physical keyboard
  gets its own locked mask with only those two bits changed, translated
  through *its* keymap: its other locked modifiers (sticky keys' locked
  modifiers, a Shift Lock) and its pressed and latched modifiers and
  layout are kept. `wlr_keyboard_notify_modifiers()` updates that
  keyboard's LEDs itself.
- **No loops.** That call re-enters `handle_modifiers()`; a per-keyboard
  `syncing` flag, like today's `restoring` for sticky keys, makes the
  re-entry only forward the modifiers to the seat. A propagation that
  changes nothing is skipped, so two keyboards never bounce an update.
- **Sticky keys first.** The sticky keys' re-application of latched and
  locked modifiers runs before the sync and is untouched by it; the sync
  never clears a bit it doesn't own.
- Virtual keyboards neither feed nor receive the shared state: they send
  their own modifiers.

- **A new keymap** (a layout or option changed in Settings) starts with
  nothing locked: the compositor puts the seat's locks back on it at once,
  so changing the layout doesn't turn Num Lock off.

The layout itself stays per keyboard, as today (see §14).

### 8.2 The warning in password fields

*Caps Lock is on* under the password field, in the caution color, while
the field has the focus and Caps Lock is on, from the real state:

- **vela-lock** already tracks `wl_keyboard.modifiers`; it tests the
  keymap's `Lock` modifier in the locked mask and draws the line under
  its field.
- **The polkit dialog** stops guessing from letters: it asks Qt's
  `wl_seat` (`QNativeInterface::QWaylandApplication::seat()`) for a second
  `wl_keyboard` (`polkit/prompt/capslock.h`), whose `keymap` and
  `modifiers` events give the same state. It asks only once Qt has a
  keyboard of its own, since `wl_seat.get_keyboard` on a seat that never
  had one is a protocol error that would end the prompt; otherwise it
  asks when a keyboard first gives a window the focus.

Both say *Caps Lock is on.* in the dark theme's caution color under the
field (vela-lock draws over a darkened wallpaper whatever the theme), and
under the error message when there is one.

### 8.3 The on-screen display

When the shared Lock state changes because of a key press, the compositor
sends the shell `caps-lock on` or `caps-lock off`. The shell shows a
small translucent pill, at the bottom in the middle like the volume
display (`shell/qml/CapsLockOsd.qml`), on the primary output, with
Breeze's `keyboard-caps-locked` or `keyboard-caps-disabled` icon and
*Caps Lock on* or *Caps Lock off*. The two take each other's place: the
one that appears closes the other. It ignores the pointer, is an alert for
screen readers, and fades after 1.5 seconds (scaled with Vela's animation
duration).

- It follows the **Lock state, not the key**: with Caps Lock mapped to
  Ctrl, Esc, Backspace, Compose or nothing, Lock never changes and no
  display appears; with a key mapped *to* Caps Lock, it does.
- Restoring state (§7) and syncing keyboards (§8.1) never show it: they
  go through `set_locks()`, whose `syncing` re-entry shares nothing.
- A **virtual keyboard** (an on-screen keyboard, a remote desktop,
  `vela-input`) doesn't share its locks (§8.1), but a change of its own
  Caps Lock shows the display too: its user pressed a key.
- **On the lock screen** the shell's layers are hidden by design
  (`compositor/src/lock.c`), so the compositor sends nothing and
  `vela-lock` draws the same pill on its own surface, on the clock and on
  the password, 48 from the bottom (no taskbar there): it takes the first
  `wl_keyboard.modifiers` as where Caps Lock starts and shows each change
  after it, whole for 1.5 seconds, then fading in the shell's exit
  duration (200 ms), both times `animation-duration`; without animation
  effects it disappears at once after 1.5 seconds.
- A switch, *Show Caps Lock on screen* in Keyboard > Typing
  (`caps-lock-osd=yes`), turns it off. The compositor reads it with the
  other keyboard keys when Settings asks it to reload; `vela-lock` when it
  starts.

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
   so Win and Vela's combinations reach it instead of acting; the media
   keys and the consoles never reach it, being reserved (§9.3). A
   modifier alone keeps it waiting; Esc alone stops recording. The key
   goes through the matcher of §9.2 with the keymap and active group of the Wayland seat, so the combination is stored as the compositor
   will match it, and shown as the first layout prints it (Win+Shift+K,
   Ctrl+Alt+1 on AZERTY too, Ctrl+Alt+Ò on Italian).
3. **Save**, enabled once both are set and the combination is allowed.
   A combination without Win asks to confirm (§9.3).

The page is `settings/qml/pages/KeyboardShortcutsPage.qml`, its model
`settings/src/customshortcuts.h`.

### 9.2 Matching a key

A combination is **modifiers + a key**. Both Vela's shortcuts and the
custom ones are matched by one function in `common/` (C, linked by the
compositor and by Settings), so what Settings accepts is exactly what
the compositor will run:

- **Modifiers are exact.** The effective Ctrl, Alt, Shift and Win (held
  or latched by sticky keys) must equal the combination's; Caps Lock, Num
  Lock and the layout are ignored. AltGr (level 3) is not a shortcut
  modifier: AltGr combinations type characters. Today's "Win and
  anything else" matches (§3) go away: Win+Shift+E no longer opens Files,
  and becomes free for a custom shortcut. Each of Vela's shortcuts that
  takes Shift (Alt+Shift+Tab, Win+Shift+S…) says so in the table.
- **Letters by their symbol.** The key's level-1 keysym in the active
  layout, without Shift, so Ctrl+Z is where the Z is printed (AZERTY,
  QWERTZ); the other printable Latin-1 symbols too (ü, ñ, +, -). When
  the active layout gives no Latin-1 symbol for that key
  (Russian, Greek, Arabic), the level-1 keysym of the first installed
  layout that does is used, as GNOME and KDE do; with none, the `us`
  layout's.
- **Other keys by position.** The digit row (as Win+number already is,
  `super_numbers.c`), the function keys, Enter, Space, Tab, Esc, the
  arrows, the keypad and the special keys are matched by key code, so
  Shift+1 is *Shift and the 1 key* whatever it types, on AZERTY too.
- **Several layouts.** Matching uses the layout active when the key is
  pressed; the Latin fallback makes a letter combination reachable from
  any of them.

The file (§9.4) stores letters as their lowercase keysym name and the
other keys by their XKB key name (`AE01` for the 1 key, `FK05`, `RTRN`),
which is position, not symbol. Settings shows each with the symbol the
first installed layout prints on it.

**One table.** Vela's own shortcuts move out of the `if`s of
`bindings.c` into a table in `common/shortcuts/table.c` of {stage,
modifiers, key, condition, action}: the condition covers the modes
(nested-only Alt variants, the Win+number mode, the color-filter switch,
the magnifier and Alt+Tab being on), the stage when the compositor
checks the row (§9.3). The compositor runs the table
(`compositor/src/bindings.c`); Settings asks the same matcher whether a
combination is taken, in the current modes. The 0.3 editor will make the
table editable; here it only replaces the `if`s, with a test that every
row of `docs/shortcuts.md` resolves to its action
(`compositor/tests/shortcuts_test.cpp`). Rows run elsewhere are listed
too, so Settings knows they are taken: Win+number (`keyboard.c`, for its
releases) and Win+Space (XKB's `grp:win_space_toggle`). Print is Print
alone now, as the other rows.

The test found one row that never ran: the nested Alt+Ctrl+F4 (close the
desktop) is the fourth console's combination, matched first; the table
leaves it out.

### 9.3 What can be used

Refused, with the reason under the combination:

- **No modifier:** a plain key or Shift with a key would stop typing it.
  F13–F24 and the special keys Vela doesn't use (Calculator, Mail,
  Favorites…) are allowed alone.
- **Vela's own:** anything the table matches in the current modes.
- **Reserved:** Ctrl+Alt+F1–F12, Win+L, Alt+Shift+Esc, the media and
  volume keys. They are matched before everything else, so they can't be
  taken: the consoles and the media keys also on the lock screen and
  while an app inhibits shortcuts (stage *always*), Win+L and
  Alt+Shift+Esc not then but while the polkit dialog waits (stage
  *before the dialog*), as before this change.
- **Already a custom shortcut:** Settings offers to replace it.

Settings counts the magnifier's keys (Win+minus, Win+Esc) as taken
whether or not it is on, since it may be when the shortcut is used;
Alt+Tab's Esc is not, the switcher being a moment.

**Warned about**, with a prominent caution bar that stays while the
combination is shown, and a confirmation on *Save*: any combination
without Win. Ctrl, Alt and Ctrl+Shift combinations are what apps use
(Ctrl+Alt+T, Ctrl+Shift+Esc…), and a custom shortcut takes them from
every app, without exception.

### 9.4 The file

`$XDG_CONFIG_HOME/vela/shortcuts.conf`, written atomically by Settings,
one line per shortcut, the combination with its modifiers in a fixed
order (Ctrl, Alt, Shift, Super) and the key as in §9.2:

```ini
# Vela's custom shortcuts (Settings > Keyboard > Custom shortcuts)
Ctrl+Alt+t=app org.kde.konsole.desktop
Shift+Super+k=command kcalc
Super+FK01=open https://docs.example.org/
```

The kinds are `app` (a desktop file ID), `command` (a shell command line)
and `open` (a URL or an absolute path). A line is at most 4096 bytes and
holds no control characters; Settings refuses a command or target that
would break that, and the readers skip a line that doesn't parse, with a
warning in the log. The file is the user's, like `vela.conf`: whoever can
write it can already run anything as the user.

### 9.5 Running them

The compositor reads only the combinations from the file, at startup and
on `reload-config`, and matches them with the matcher of §9.2 in
`vela_bindings_handle()`, after the reserved keys, the lock, the polkit
dialog and `keyboard-shortcuts-inhibit` (so a VM or remote desktop keeps
them). It never puts the target on the shell's socket, whose messages
are newline-delimited text: it sends `shortcut <combination>`, and a
normalized combination holds only `[A-Za-z0-9+_]`. The shell reads the
file itself, each time it is asked (`shell/src/shortcutrunner.cpp`; the
file is small, and so never stale), finds the line and runs it as
Start would: an app with an activation token, so its window comes forward
under the focus rules of
[#264](https://github.com/vela-desktop/vela/pull/264); a command with
`sh -c`, detached; a link or file with its default app. A combination
the shell can't find (the file changed in between) does nothing and is
logged. The compositor logs the lines it skips, with their number.

## 10. The Win key alone

`super-opens-start=yes|no`, Settings > Keyboard > Typing, *Open Start
with the Win key*, on by default. Off, a press and release of Win alone
does nothing; Win combinations, the taskbar numbers while Win is held and
the Start button are unchanged. In a nested session Alt+S stays.

## 11. The terminal

Win+Enter and the updater use the packaged `xdg-terminal-exec` launcher.
It reads the desktop entries, honors profile actions and selects the
terminal-specific arguments for running the updater's command. The updater
keeps its window open until the user presses Enter.

Settings > Apps > Default apps lists terminal emulators from the application
catalog. Selecting one prepends its desktop ID to
`$XDG_CONFIG_HOME/vela-xdg-terminals.list`. Choosing *Automatic* removes
the entry Settings wrote and preserves the user's own preferences and
directives. The session's `XDG_CURRENT_DESKTOP` starts with Vela, so the
choice applies to Vela sessions.

An explicit preference in Vela's list takes priority. With no preference,
a runnable `VELA_TERMINAL` command is used for compatibility (with `-e`
for the updater). Otherwise the XDG launcher selects the default. Settings
asks it for the selected ID asynchronously when it supports `--print-id`.

## 12. Configuration

New and changed `vela.conf` keys, documented in `docs/configuration.md`:

| Key | Default | Meaning |
|---|---|---|
| `keyboard-options` | empty | XKB options. Settings owns those of §6 and keeps the others. |
| `numlock` | `off` | `off`, `on` or `last`: Num Lock when Vela starts (§7). |
| `caps-lock-osd` | `yes` | The Caps Lock display (§8.3). |
| `super-opens-start` | `yes` | Win alone opens Start (§10). |

Other files: `$XDG_STATE_HOME/vela/numlock` (§7),
`$XDG_CONFIG_HOME/vela/shortcuts.conf` (§9.4),
`$XDG_CONFIG_HOME/vela-xdg-terminals.list` (§11).

## 13. Stages

One pull request, one commit per stage; each commit updates the user
documentation it changes (`docs/shortcuts.md`, `docs/configuration.md`,
`docs/features.md`, `CHANGELOG.md`).

| Stage | Contents |
|---|---|
| KP1 | The Keyboard section (§4), the layout's source (§5.5), no `kxkbrc` (§5.6). Done. |
| KP2 | The catalogue from libxkbregistry, names, search and the four-layout limit (§5.1, §5.2). Done. |
| KP3 | Flags (§5.3). Dropped. |
| KP4 | Try before adding: the drawing, the try field, *Preview* (§5.4). Done. |
| KP5 | Key behavior (§6). Done. |
| KP6 | One lock state for every keyboard and Num Lock at startup (§7, §8.1). Done. |
| KP7 | The Caps Lock warning in vela-lock and the polkit dialog (§8.2). Done. |
| KP8 | The Caps Lock display, in the shell and vela-lock (§8.3). Done. |
| KP9 | The binding table and matcher, with Vela's shortcuts moved into it (§9.2); then custom shortcuts: file, compositor, shell, Settings page (§9). Done. |
| KP10 | The Win key alone (§10). Done. |
| KP11 | The terminal lookup and Default apps > Terminal (§11). Done. |

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

- Catalogue: libxkbregistry over a fixture include path (base, extras,
  user rules) gives every layout, grouped, with countries and languages;
  the four-layout limit.
- Names and search: translated and English names, codes, languages,
  countries, accents; a group expands to its matching variants.
- Drawing: each shape's rows are 15 units wide; the usual shape of
  `it`, `us`, `jp` and `br`; a letter's capital alone, names on modifier
  keys, AltGr detected (not for `us`); options move the names (Ctrl on
  LALT with `ctrl:swap_lalt_lctl`).
- Try field: a keymap compiled for `it` turns the key codes of *ò*, *à*
  and AltGr+*è* into those characters; a dead key and a Compose sequence
  for `us(intl)`; an auto-repeated key types again without a double state
  update; each character is inserted once whatever the layout in use;
  focus loss releases held modifiers.
- Options: each control writes its option, keeps unknown ones, shows
  *Custom* for a conflicting one; Compose on Caps disables the Caps
  control.
- Num Lock: the state for each mode and saved file; the file is written
  once after a burst, at exit, and atomically (a restart right after a
  change reads it; a truncated file reads as *off*).
- Lock sync: two keyboards with *different* keymaps (Num Lock on another
  modifier index) share Caps and Num Lock; their other locked, pressed and
  latched modifiers, sticky keys' among them, and their layouts are kept;
  no re-entry loop; virtual keyboards untouched.
- Matcher: exact modifiers (Win+Shift+E is not Win+E); letters on AZERTY
  and QWERTZ by symbol; a letter combination with `ru` active through the
  Latin fallback; the digit row and F-keys by position (Shift+1 on
  AZERTY); AltGr never a modifier; every row of `docs/shortcuts.md`
  resolves to its action in its mode; Settings' conflict answer equals
  the compositor's for every combination of the table.
- Shortcuts file: parser, normalization, the 4096-byte and
  control-character limits; the shell message holds only the combination.
- Terminal lookup: the user list, `VELA_TERMINAL`, and delegation to
  `xdg-terminal-exec`, including desktop actions and argument boundaries.

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
