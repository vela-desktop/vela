# On-screen keyboard

How Vela types without a physical keyboard: the on-screen keyboard, how what
it types reaches the apps, when it appears, and the groundwork for input
methods (Chinese, Japanese, Indian languages). This document comes before the
code. It lists where Vela is today, the decisions already taken, the
invariants the code must keep, and the changes in the order they are made,
with how each is verified.

> **Status:** draft, under discussion. Nothing is implemented. The open
> questions are in §9.

Touch support ([#94](https://github.com/vela-desktop/vela/pull/94),
`docs/touch.md`) depends on this work: a device without a keyboard cannot even
unlock. Its requirements for the keyboard (its §8) are all covered here.

## 1. Goals

- **Type anywhere.** Every app, Wayland or X11, receives what the on-screen
  keyboard types, and so do the lock screen and the polkit dialog.
- **Out of the way.** It appears by itself only when a text field is touched;
  on a laptop with a keyboard it never shows up uninvited.
- **The user's layout.** The keys show and type what the active keyboard
  layout says: an Italian layout has à, è, ì, ò, ù.
- **Ready for input methods.** Chinese, Japanese and Indian languages can be
  added later, through the standard protocols, without redesigning anything.
- **Only Vela types.** No app can inject keys the way the keyboard does.

## 2. Decisions taken

1. **Hybrid.** What is a key on the active layout is sent as a key, through the
   same path as a physical keyboard. What is not a key (emoji, a variant that
   isn't on the layout) is sent as text over `text-input-v3`. This keeps input
   methods such as fcitx5 and ibus working (§5.12), which a keyboard that
   is itself the input method would exclude.
2. **A separate process, `vela-onscreen-keyboard`, in C++ with Qt Quick.**
   It uses Vela's controls and theme, so it looks and moves like the rest of
   the desktop, and gets fonts, emoji and CJK glyphs from Qt. The compositor
   starts it, gives it a private connection, and restarts it if it crashes.
3. **It appears by itself** when a text field gets the focus from a touch or a
   pen, like GNOME, and **by hand** from a button in the taskbar's
   notification area, a shortcut, or an edge gesture. There is always a way to
   close it.
4. **The taskbar button is a setting:** always, only with a touchscreen (the
   default), or never.
5. **Docked at the bottom by default, can be undocked,** and remembers how it
   was the last time. It **overlaps** the windows: it never resizes them. A
   window whose text cursor would end up under it slides up (§5.6).
6. **Compact layout by default.** The full layout is always exactly two taps
   away. The field's type chooses a layout (digits, email, URL), with a
   one-tap way back.
7. **No autocorrection and no word suggestions.** The only candidates ever
   shown are an input method's.
8. **Input methods are prepared, not a priority** (§5.12).

## 3. Where Vela is today

- **No text input protocols.** The compositor implements neither
  `text-input-v3` nor `input-method-v2`: apps can't say that a text field is
  focused, and input methods such as fcitx5 and ibus don't work.
- **Apps are ready.** Qt 6, GTK, Firefox and Chromium speak `text-input-v3`.
  Vela's `TextBox` already marks password fields (`ImhHiddenText`,
  `ImhSensitiveData`). X11 apps have no equivalent.
- **The compositor owns the layout.** `keyboard-layout=it,us` in `vela.conf`,
  Win+Space for the next one (`keyboard.h`), with every key going through one
  path: sticky keys, Super alone for Start, shortcuts, then the focused app.
- **Every protocol is open to every client.** Vela has no global filter. The
  virtual keyboard and pointer exist only with `VELA_DEBUG_INPUT=1`, for tests.
- **A private connection already exists.** The compositor starts `vela-lock`
  with its own connection (`VELA_WAYLAND_SOCKET_FD`).
- **Shell panels close when they lose activation** (`onActiveChanged` in Start,
  Quick Settings, Notification Center, clipboard history…). A keyboard that
  took the focus would close the panel being typed into.
- **Win+Ctrl+O is free**, the shortcut Windows uses for its on-screen keyboard.

## 4. Invariants

Every change in §5 follows from these.

1. **Only Vela's keyboard types.** The protocol that injects keys and text
   exists only on the keyboard's private connection.
2. **Keys are keys.** Everything sent as a key goes through the same path as a
   physical key: the layout, sticky keys, shortcuts, an input method's grab,
   the focused app.
3. **The keyboard never takes the focus.** A press on it changes neither the
   keyboard focus, nor the active window, nor the stacking, and it closes
   neither the shell's panels nor the app's menus.
4. **The clipboard is the user's.** The keyboard never writes to it in order
   to type.
5. **Passwords leave no trace.** In password fields there are no enlarged key
   previews (they show in screen sharing) and nothing is recorded, not even
   recent emoji.
6. **It doesn't get in the way.** It appears by itself only after a touch, and
   a key on a physical keyboard hides it if it appeared by itself.
7. **Locked means locked.** While the screen is locked the keyboard types only
   into the lock screen and shows neither the clipboard nor recent emoji.
8. **No use, no cost.** The process starts the first time the keyboard is
   needed; with the button set to never and no touchscreen, it never runs.

## 5. Design

### 5.1 The pieces

| Piece | Role |
|---|---|
| Compositor | `text-input-v3`; an internal keyboard device for the on-screen keys; the private `vela-onscreen-keyboard-v1` protocol; showing, hiding and placing the keyboard; later, `input-method-v2` (§5.12) |
| `vela-onscreen-keyboard` | Draws the keyboard on a layer-shell surface (overlay layer, no keyboard interactivity), turns taps into keys or text |
| Shell | The taskbar button; Settings has the settings (§5.13) |
| `vela-lock` | `text-input-v3` on its own connection, and a keyboard button (§5.11) |

### 5.2 The private connection

The compositor starts `vela-onscreen-keyboard` on demand (invariant 8) with
one end of a socket pair in `WAYLAND_SOCKET`, which libwayland and therefore
Qt use instead of the public socket. A global filter
(`wl_display_set_global_filter`), Vela's first, shows
`vela-onscreen-keyboard-v1` to that client only. If the process exits, the
compositor restarts it the next time the keyboard is needed, with a limit on
repeated crashes like the shell's.

The protocol, as a first sketch (the exact messages are part of the first
stage):

- **compositor → keyboard:** show (on which output, why: touch, button,
  shortcut, gesture), hide; the field's content type and hints; the keymap and
  the active layout; the modifiers' state; whether the focused field accepts
  text (`text-input-v3` enabled); whether the screen is locked.
- **keyboard → compositor:** a key pressed or released (evdev keycode); some
  text to commit; switch layout; hide; the area it covers (for §5.6).

### 5.3 How keys and text reach the apps

**Keys.** The compositor creates its own `wlr_keyboard`, "Vela on-screen
keyboard", with the same keymap and active layout as the physical keyboards,
and emits the keyboard's keys on it. They follow the path of every key
(invariant 2): Super alone opens Start, Ctrl+C copies, sticky keys apply, an
input method's grab receives them. X11 apps receive them like any other key.

**Text** that isn't a key on the layout:

1. If the focused field has `text-input-v3` enabled, the compositor sends it as
   a commit string.
2. Otherwise (X11, apps without `text-input-v3`), the compositor puts the
   character on a spare keycode of the on-screen keyboard's keymap and sends
   that key, as tools like `wtype` do.
3. Never through the clipboard (invariant 4).

### 5.4 `text-input-v3` in the compositor

The compositor tracks the text input of the focused surface: enabled or not,
content purpose and hints, and the cursor rectangle. That is enough to show
and hide the keyboard, choose its layout (§5.7) and keep the cursor visible
(§5.6), even without an input method. When one is present, the same state is
relayed to it (§5.12).

### 5.5 When it appears and disappears

| Event | The keyboard |
|---|---|
| A text field gets the focus from a touch or a pen, and showing by itself is on | appears (*automatically*) |
| The taskbar button, Win+Ctrl+O, the edge gesture | appears or hides (*by hand*) |
| Tablet mode, any text field focused | appears (*automatically*; with tablet mode in touch's last stage) |
| The focused field is left, or disabled | hides, if it appeared automatically |
| A key on a physical keyboard | hides, if it appeared automatically |
| The hide key, or a swipe down on its top edge | hides |

Opened by hand, the keyboard stays until it is closed: the user asked for it.
X11 apps can't say they have a text field, so for them the keyboard only opens
by hand.

The keyboard opens on the output of the focused window, or, by hand, on the
output where the button or the gesture was.

The compositor knows which device made the last press on the seat; "from a
touch" means the last press before the field was enabled came from a
touchscreen or a pen. Touch doesn't exist yet: this rule, and its tests, land
with touch's first stage (§8).

The edge gesture is the `keyboard` action that touch adds to the actions an
edge can have.

### 5.6 Docked, floating, and the windows under it

**Docked:** across the whole bottom of the output, over the taskbar, on the
overlay layer, with no exclusive zone. Its height depends on the layout and
the output's orientation.

**Floating:** narrower, moved by a handle on its top edge. One button docks
and undocks it.

**Remembered:** docked or floating, the floating position per output, and the
last layout chosen by hand are saved in `$XDG_STATE_HOME/vela/onscreen-keyboard`
and restored at the next opening. This is state, not a setting, so it isn't in
`vela.conf`.

**Keeping the text cursor visible.** When the keyboard appears or the cursor
moves, if the focused text input's cursor rectangle falls under the area the
keyboard covers, the compositor slides that window up by just enough, with
Vela's motion, and slides it back when the keyboard hides or the focus moves.
Maximized and fullscreen windows slide too. Apps that don't report the cursor
are not moved.

### 5.7 Layouts

- **Compact** (default): the letters, a bottom row with `?123`, emoji, space,
  Enter, and the layout button. Symbols and digits are on a second page.
- **Full:** a PC keyboard with Esc, Tab, Ctrl, Win, Alt, the arrows, Delete and
  the number row; F1–F12 behind Fn.
- **Number pad.**

**Exactly two taps.** The layout button is on every layout: one tap opens the
choice, the second picks Compact, Full or Number pad.

**By the field's type.** The content purpose chooses where the keyboard
starts: digits, number, phone and PIN open the number pad; email adds `@` and
`.`; URL adds `/` and `.com`; terminal opens the full layout; password opens
compact with the rules of invariant 5. The way back is one tap (`ABC` on the
number pad) and the layout button is always there. A layout chosen because of
the field is not remembered as the user's choice.

**Labels from the layout.** Each key is a position on a standard keyboard, and
its label is what the compositor's keymap gives for that keycode in the active
layout and shift level (xkbcommon). Italian, German, Russian or Greek layouts
work without a table per language. With more than one layout, a globe key does
what Win+Space does.

### 5.8 Keys

- **Tap:** press and release. An enlarged preview of the key appears above the
  finger, except in password fields.
- **Press and hold on a letter:** the variants (à, á, â…). A variant that is a
  key on the layout is sent as a key, any other as text (§5.3).
- **Press and hold on Backspace, the arrows and Space:** repeat, with the
  keyboard's delay and rate from Settings.
- **Shift:** one tap for the next letter, two for Caps Lock. Ctrl, Alt and Win
  (on the full layout) stay pressed for the next key, so Ctrl+C is two taps;
  two taps keep them pressed.

### 5.9 Panels

- **Emoji:** a panel in place of the keys, with search and recent emoji (none
  recorded from password fields). Emoji are text (§5.3).
- **Clipboard history:** the history Vela already has, shown in the keyboard;
  hidden while locked (invariant 7).

### 5.10 Focus, the shell and the app's menus

The keyboard's surface has no keyboard interactivity, and the compositor
treats a press on it as outside every focus rule (invariant 3):

- the focused window stays focused and active, and keeps its place in the
  stack;
- the shell's panels don't lose activation, so Start's search can be typed
  into from the keyboard;
- an app's popup (a menu, a combo box with a search field) is not dismissed as
  by a click outside.

### 5.11 The lock screen and the polkit dialog

While the screen is locked, the compositor shows the keyboard's surface above
the lock surfaces; it is the only other surface shown. `vela-lock` implements
`text-input-v3` on its own connection, with the password purpose, and draws a
keyboard button in a corner, like Windows' lock screen. The keyboard knows
that the screen is locked and hides the clipboard and recent emoji.

The polkit dialog is a Qt app with `text-input-v3`; the keyboard appears above
it like above any window.

### 5.12 Input methods (prepared, not a priority)

What makes Chinese, Japanese and the Indian languages possible later:

- **Relay.** The compositor relays `text-input-v3` to `input-method-v2`:
  activation, surrounding text, content type, commits, preedit, deletions.
- **Keyboard grab.** An input method can grab the keyboard; the on-screen
  keyboard's keys reach it like physical keys (invariant 2), so pinyin or
  romaji typed on the screen become characters.
- **Candidates.** The input method's popup surface is placed at the text
  cursor, above the on-screen keyboard.
- **Security.** An input method sees every key, so it is not open to any
  client. The input method chosen in Settings (fcitx5 or ibus) is started by
  Vela on a private connection like the keyboard's, and only that connection
  sees `input-method-v2` and `virtual-keyboard` (which input methods use to
  pass on the keys they don't consume).

The on-screen keyboard keeps showing the Latin layout under an input method;
the candidates are the input method's (decision 7).

### 5.13 Settings

In Settings › Keyboard, a section "On-screen keyboard":

| Setting | `vela.conf` | Values | Default |
|---|---|---|---|
| Keyboard button in the taskbar | `onscreen-keyboard-button=touch` | `always`, `touch`, `never` | `touch` |
| Show when a text field is touched | `onscreen-keyboard-auto=yes` | `yes`, `no` | `yes` |

`touch` shows the button only while a touchscreen exists. Win+Ctrl+O works with
any setting and is added to `docs/shortcuts.md`. Later, Settings ›
Keyboard also chooses the input method (§5.12).

## 6. Not in scope

- Autocorrection and word suggestions (decision 7).
- Swipe typing and dictation.
- Key sounds and vibration.
- A split layout for large tablets.
- Candidates drawn inside the on-screen keyboard: input methods draw their own.
- Mouse-only accessibility (dwell clicking, scanning), as in Windows' `osk`.

## 7. Tests

**Unit tests (GoogleTest).** The content purpose to layout mapping; labels for
it, us, de and ru keymaps from xkbcommon; whether a variant is a key or text;
the show and hide rules of §5.5 as a state machine; the window slide of §5.6;
the global filter (which client sees which global).

**Functional tests.** A test client with `text-input-v3` that sets a content
purpose and reports what it receives: keys, commits, preedit. To tap the
keyboard, `vela-input` clicks; with `VELA_DEBUG_INPUT=1`
`vela-onscreen-keyboard` writes its keys' rectangles to the runtime directory,
so tests find them without reading pixels. The scenarios:

- Win+Ctrl+O and the taskbar button open and close the keyboard; the button
  follows `onscreen-keyboard-button`;
- tapping keys types into the client, with the it and us layouts, and the
  labels follow Win+Space;
- Shift once and twice, Ctrl+C with two taps, Backspace repeating;
- the full layout in two taps, from every layout;
- a number field opening the number pad, and `ABC` going back;
- an emoji reaching a client as a commit, and an X11 window or a client
  without `text-input-v3` through a spare keycode; the clipboard unchanged;
- a window whose cursor would be covered sliding up and back;
- tapping the keyboard with Start's search open: Start stays open and
  receives the text; an app's popup stays open;
- docked, floating and the last layout restored after the process restarts;
- a crashed keyboard restarted;
- the lock screen: the keyboard shown above it, a password typed and the
  screen unlocked; no clipboard history;
- a client that is not the keyboard cannot see `vela-onscreen-keyboard-v1`;
- with touch: the keyboard appearing for a touched field and not for a
  clicked one, and hiding at a physical key.

## 8. Stages

Each stage is one pull request with its tests.

| Stage | Content | Sections |
|---|---|---|
| **K0** | `text-input-v3`, the global filter and the private connection, the internal keyboard device, `vela-onscreen-keyboard-v1`, the test client | §5.1–§5.4 |
| **K1** | `vela-onscreen-keyboard`: compact and full layouts, labels from the layout, keys, Shift and modifiers, repeat; opening by hand (button, Win+Ctrl+O), hiding; docked; the settings | §5.5, §5.7, §5.8, §5.13 |
| **K2** | Floating, the remembered state, the window slide, layouts by field type, the number pad | §5.6, §5.7 |
| **K3** | Variants, the emoji and clipboard panels, text commits and the spare-keycode fallback | §5.3, §5.8, §5.9 |
| **K4** | The lock screen and the polkit dialog | §5.11 |
| **K5** | With touch's first stage: showing by itself after a touch, hiding at a physical key | §5.5 |
| **I1** | Input methods: the relay, the grab, the candidates, the input method started by Vela, its choice in Settings | §5.12 |

K0–K4 come before touch; K5 lands with it. I1 is not scheduled.

## 9. Open questions

- **The spare-keycode fallback** changes the keymap for a moment; X11 apps may
  be slow to follow. Is it worth it, or should emoji simply not be offered
  where `text-input-v3` is missing?
- **Input methods on a private connection.** If fcitx5 or ibus open more than
  one Wayland connection, `WAYLAND_SOCKET` covers only one; the alternative is
  recognizing them by their executable, which is weaker.
- **Sizes:** the docked height in landscape and portrait, the key sizes, and
  the floating keyboard's default width.
- **Rotation:** whether a floating keyboard keeps its position when the output
  rotates or goes back to docked.
- **The window slide** for fullscreen games, where moving the window may be
  worse than covering the field.
