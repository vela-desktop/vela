// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "super_numbers.h"

#include <linux/input-event-codes.h>
#include <string.h>

enum vela_super_numbers vela_super_numbers_parse(const char *mode)
{
    if (mode && strcmp(mode, "desktops") == 0) {
        return VELA_SUPER_NUMBERS_DESKTOPS;
    }
    if (mode && strcmp(mode, "off") == 0) {
        return VELA_SUPER_NUMBERS_OFF;
    }
    return VELA_SUPER_NUMBERS_APPS;
}

int vela_super_number_index(uint32_t keycode)
{
    return keycode >= KEY_1 && keycode <= KEY_0 ? (int)(keycode - KEY_1) : -1;
}

enum vela_super_number_action vela_super_number_action(enum vela_super_numbers mode, struct vela_super_modifiers mods)
{
    // Ctrl and Alt+Shift stay the apps' (and Super+Ctrl the desktops').
    if (mode == VELA_SUPER_NUMBERS_OFF || !mods.logo || mods.ctrl || (mods.alt && mods.shift)) {
        return VELA_SUPER_NUMBER_NONE;
    }
    if (mode == VELA_SUPER_NUMBERS_DESKTOPS) {
        return mods.alt ? VELA_SUPER_NUMBER_NONE : mods.shift ? VELA_SUPER_NUMBER_MOVE : VELA_SUPER_NUMBER_SWITCH;
    }
    return mods.shift ? VELA_SUPER_NUMBER_NEW : mods.alt ? VELA_SUPER_NUMBER_JUMP : VELA_SUPER_NUMBER_ACTIVATE;
}

const char *vela_super_number_verb(enum vela_super_number_action action)
{
    switch (action) {
    case VELA_SUPER_NUMBER_ACTIVATE: return "activate";
    case VELA_SUPER_NUMBER_NEW: return "new";
    case VELA_SUPER_NUMBER_JUMP: return "jump";
    default: return NULL;
    }
}

size_t vela_super_numbers_filter(const uint32_t *keycodes, size_t count, uint16_t handled, uint32_t *out)
{
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        const int index = vela_super_number_index(keycodes[i]);
        if (index < 0 || !(handled & (1u << index))) {
            out[kept++] = keycodes[i];
        }
    }
    return kept;
}
