// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_LOCK_KEYS_H
#define VELA_LOCK_KEYS_H

// Caps Lock and Num Lock as the seat's, not each keyboard's
// (docs/keyboard.md §7, §8.1), and Num Lock when Vela starts. Pure logic:
// keyboard.c applies it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct xkb_keymap;

// numlock= in vela.conf: off (the default), on, or as it was at the end of
// the last session.
enum vela_numlock {
    VELA_NUMLOCK_OFF,
    VELA_NUMLOCK_ON,
    VELA_NUMLOCK_LAST,
};
enum vela_numlock vela_numlock_parse(const char *value);

// Caps Lock and Num Lock in a keyboard's locked modifiers, by the
// modifiers' names in its keymap, so keyboards with different keymaps
// agree.
void vela_locks_read(struct xkb_keymap *keymap, uint32_t locked, bool *caps, bool *num);
// `locked` with only the Caps Lock and Num Lock bits set as asked: the
// other locked modifiers (a sticky key's, a Shift Lock) stay.
uint32_t vela_locks_apply(struct xkb_keymap *keymap, uint32_t locked, bool caps, bool num);

// Where Num Lock's last state is kept: $XDG_STATE_HOME/vela/numlock (or
// ~/.local/state/vela/numlock). false if neither is usable.
bool vela_numlock_path(char *out, size_t size);
// The state kept in `path`: on only if the file says "on"; missing,
// unreadable or anything else is off.
bool vela_numlock_load(const char *path);
// Keeps `on` in `path` atomically (a temporary file, fsync, rename),
// creating its directory. false on failure.
bool vela_numlock_save(const char *path, bool on);
// Num Lock when the first keyboard appears.
bool vela_numlock_initial(enum vela_numlock mode, const char *path);

#endif
