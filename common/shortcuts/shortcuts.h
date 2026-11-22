// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SHORTCUTS_H
#define VELA_SHORTCUTS_H

// A key combination and the key that was pressed, matched the same way by
// the compositor and by Settings (docs/keyboard.md §9.2): what Settings
// accepts is exactly what the compositor runs. C17 and xkbcommon only.
//
// - The modifiers are exact: Ctrl, Alt, Shift and Win, held, latched or
//   locked (sticky keys), must equal the combination's. Caps Lock, Num
//   Lock, AltGr and the layout don't count.
// - Letters and the other printable keys by their symbol: the key's
//   level-1 keysym in the layout in use, lowercase, so Ctrl+Z is where the
//   Z is printed. Printable is Latin-1 (ü and ñ too). A layout without one
//   for that key (Russian, Greek) borrows it from the first of the keymap's
//   layouts that has one, or from `us`.
// - The other keys by position (their key code): the digit row, the
//   function keys, Enter, Space, Tab, Esc, the arrows, the keypad and the
//   special keys, so Shift+1 is Shift and the 1 key, on AZERTY too.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct xkb_keymap;
struct xkb_state;

enum {
    VELA_MOD_CTRL = 1 << 0,
    VELA_MOD_ALT = 1 << 1,
    VELA_MOD_SHIFT = 1 << 2,
    VELA_MOD_SUPER = 1 << 3,
};

enum vela_key_kind {
    VELA_KEY_SYM, // the level-1 keysym, lowercase (letters, punctuation)
    VELA_KEY_CODE, // the evdev key code (position)
    VELA_KEY_TYPED, // the keysym the key types now (XF86Switch_VT_n)
};

// A pressed key, as the matcher sees it.
struct vela_press {
    uint8_t mods; // VELA_MOD_*
    uint32_t keycode; // evdev (xkb's minus 8)
    uint32_t sym; // level-1, lowercase, with the Latin fallback
    uint32_t typed; // what the key types with the modifiers held
};

struct vela_combo {
    uint8_t mods;
    enum vela_key_kind kind; // VELA_KEY_SYM or VELA_KEY_CODE
    uint32_t key;
};

// The key `xkb_keycode` pressed with `state` as it is before the key.
void vela_press_from_state(struct xkb_state *state, uint32_t xkb_keycode, struct vela_press *out);

// The modifiers of `state` that count for shortcuts.
uint8_t vela_shortcut_mods(struct xkb_state *state);

// Whether the key is matched by position: not a printable level-1 symbol,
// or a key of the digit row or the keypad.
bool vela_press_is_positional(const struct vela_press *press);

// The combination a pressed key makes, as Settings records it.
struct vela_combo vela_combo_from_press(const struct vela_press *press);

bool vela_combo_matches(const struct vela_combo *combo, const struct vela_press *press);

// "Ctrl+Alt+t", "Super+Shift+k", "Super+FK01": modifiers (Ctrl, Alt, Shift,
// Super, any case and order) and a key, a lowercase keysym name for the
// printable keys or an XKB key name (AE01, FK05, RTRN) for the others.
// false if it doesn't parse.
bool vela_combo_parse(const char *text, struct vela_combo *out);

// The normalized text of a combination: the modifiers in the order Ctrl,
// Alt, Shift, Super, then the key, only [A-Za-z0-9+_]. false if `size` is
// too small or the key has no name.
bool vela_combo_format(const struct vela_combo *combo, char *out, size_t size);

// The `us` keymap the Latin fallback and the key names use, compiled once.
struct xkb_keymap *vela_shortcuts_us_keymap(void);

#endif
