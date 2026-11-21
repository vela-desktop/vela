// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SUPER_NUMBERS_H
#define VELA_SUPER_NUMBERS_H

// Super with a number key (docs/taskbar.md §7): which taskbar app or virtual
// desktop it is for, and the number keys the compositor keeps from the apps.
// Pure logic, keyboard.c and input.c apply it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum vela_super_numbers {
    VELA_SUPER_NUMBERS_APPS,
    VELA_SUPER_NUMBERS_DESKTOPS,
    VELA_SUPER_NUMBERS_OFF,
};

enum vela_super_number_action {
    VELA_SUPER_NUMBER_NONE, // not a shortcut: the key goes to the app
    VELA_SUPER_NUMBER_ACTIVATE, // Super: the app's button, as a click
    VELA_SUPER_NUMBER_NEW, // Super+Shift: a new window of the app
    VELA_SUPER_NUMBER_JUMP, // Super+Alt: the app's jump list
    VELA_SUPER_NUMBER_SWITCH, // Super (desktops): to that desktop
    VELA_SUPER_NUMBER_MOVE, // Super+Shift (desktops): the active window there
};

struct vela_super_modifiers {
    bool logo;
    bool ctrl;
    bool alt;
    bool shift;
};

// The super-number-keys value of vela.conf: "apps" when unknown.
enum vela_super_numbers vela_super_numbers_parse(const char* mode);

// The key's index, 0 for 1 up to 9 for 0; -1 for any other key.
int vela_super_number_index(uint32_t keycode);

enum vela_super_number_action vela_super_number_action(enum vela_super_numbers mode, struct vela_super_modifiers mods);

// The shell's word for an app action ("activate", "new", "jump"); NULL for
// the others.
const char* vela_super_number_verb(enum vela_super_number_action action);

// `keycodes` without the number keys in `handled` (bit i: index i), into
// `out` (room for `count`). Returns how many are left.
size_t vela_super_numbers_filter(const uint32_t* keycodes, size_t count, uint16_t handled, uint32_t* out);

#endif
