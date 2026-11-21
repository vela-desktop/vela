// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shortcuts.h"

#include <ctype.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <xkbcommon/xkbcommon.h>

struct xkb_keymap* vela_shortcuts_us_keymap(void)
{
    static struct xkb_keymap* keymap;
    if (!keymap) {
        struct xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!context) {
            return NULL;
        }
        const struct xkb_rule_names names = { .rules = "evdev", .model = "pc105", .layout = "us" };
        keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        xkb_context_unref(context);
    }
    return keymap;
}

// A printable Latin-1 symbol: ASCII, and ü, ñ, é… (their keysyms are their
// code points).
static bool printable(xkb_keysym_t sym)
{
    return (sym >= 0x21 && sym <= 0x7e) || (sym >= 0xa1 && sym <= 0xff);
}

static xkb_keysym_t level_one(struct xkb_keymap* keymap, xkb_keycode_t keycode, xkb_layout_index_t layout)
{
    const xkb_keysym_t* syms = NULL;
    if (!keymap || xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 0, &syms) < 1) {
        return XKB_KEY_NoSymbol;
    }
    return xkb_keysym_to_lower(syms[0]);
}

uint8_t vela_shortcut_mods(struct xkb_state* state)
{
    static const struct {
        const char* name;
        uint8_t mod;
    } names[] = {
        { XKB_MOD_NAME_CTRL, VELA_MOD_CTRL },
        { XKB_MOD_NAME_ALT, VELA_MOD_ALT },
        { XKB_MOD_NAME_SHIFT, VELA_MOD_SHIFT },
        { XKB_MOD_NAME_LOGO, VELA_MOD_SUPER },
    };
    uint8_t mods = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (xkb_state_mod_name_is_active(state, names[i].name, XKB_STATE_MODS_EFFECTIVE) > 0) {
            mods |= names[i].mod;
        }
    }
    return mods;
}

void vela_press_from_state(struct xkb_state* state, uint32_t xkb_keycode, struct vela_press* out)
{
    struct xkb_keymap* keymap = xkb_state_get_keymap(state);
    memset(out, 0, sizeof(*out));
    out->mods = vela_shortcut_mods(state);
    out->keycode = xkb_keycode >= 8 ? xkb_keycode - 8 : 0;
    out->typed = xkb_state_key_get_one_sym(state, xkb_keycode);
    const xkb_layout_index_t active = xkb_state_key_get_layout(state, xkb_keycode);
    xkb_keysym_t sym = active == XKB_LAYOUT_INVALID ? XKB_KEY_NoSymbol : level_one(keymap, xkb_keycode, active);
    // No Latin symbol here: the first layout of the keymap with one, then
    // us's.
    const xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(keymap, xkb_keycode);
    for (xkb_layout_index_t layout = 0; !printable(sym) && layout < layouts; ++layout) {
        const xkb_keysym_t other = level_one(keymap, xkb_keycode, layout);
        if (printable(other)) {
            sym = other;
        }
    }
    if (!printable(sym)) {
        const xkb_keysym_t us = level_one(vela_shortcuts_us_keymap(), xkb_keycode, 0);
        if (printable(us)) {
            sym = us;
        }
    }
    out->sym = sym;
}

bool vela_press_is_positional(const struct vela_press* press)
{
    const bool digit_row = press->keycode >= KEY_1 && press->keycode <= KEY_0;
    return digit_row || !printable(press->sym);
}

struct vela_combo vela_combo_from_press(const struct vela_press* press)
{
    const bool position = vela_press_is_positional(press);
    return (struct vela_combo) {
        .mods = press->mods,
        .kind = position ? VELA_KEY_CODE : VELA_KEY_SYM,
        .key = position ? press->keycode : press->sym,
    };
}

bool vela_combo_matches(const struct vela_combo* combo, const struct vela_press* press)
{
    if (combo->mods != press->mods) {
        return false;
    }
    switch (combo->kind) {
    case VELA_KEY_SYM: return combo->key == press->sym;
    case VELA_KEY_CODE: return combo->key == press->keycode;
    case VELA_KEY_TYPED: return combo->key == press->typed;
    }
    return false;
}

static const struct {
    const char* name;
    uint8_t mod;
} modifier_names[] = {
    { "Ctrl", VELA_MOD_CTRL },
    { "Alt", VELA_MOD_ALT },
    { "Shift", VELA_MOD_SHIFT },
    { "Super", VELA_MOD_SUPER },
    // Also understood when read.
    { "Control", VELA_MOD_CTRL },
    { "Win", VELA_MOD_SUPER },
    { "Logo", VELA_MOD_SUPER },
};

// Shaped like an XKB key name: 2 to 4 capitals and digits (ESC, AE01, RTRN).
static bool key_name(const char* token)
{
    const size_t length = strlen(token);
    if (length < 2 || length > 4) {
        return false;
    }
    for (const char* c = token; *c; ++c) {
        if (!isupper((unsigned char)*c) && !isdigit((unsigned char)*c)) {
            return false;
        }
    }
    return true;
}

// The key code (evdev) of us's key whose level-1 symbol is `sym`.
static bool us_keycode_for(xkb_keysym_t sym, uint32_t* keycode)
{
    struct xkb_keymap* us = vela_shortcuts_us_keymap();
    if (!us) {
        return false;
    }
    for (xkb_keycode_t code = xkb_keymap_min_keycode(us); code <= xkb_keymap_max_keycode(us); ++code) {
        if (code >= 8 && level_one(us, code, 0) == xkb_keysym_to_lower(sym)) {
            *keycode = code - 8;
            return true;
        }
    }
    return false;
}

static bool parse_key(const char* token, struct vela_combo* out)
{
    if (!*token) {
        return false;
    }
    // A key name, unless no key has it (F5 is a keysym).
    if (key_name(token)) {
        struct xkb_keymap* us = vela_shortcuts_us_keymap();
        const xkb_keycode_t code = us ? xkb_keymap_key_by_name(us, token) : XKB_KEYCODE_INVALID;
        if (code != XKB_KEYCODE_INVALID && code >= 8) {
            out->kind = VELA_KEY_CODE;
            out->key = code - 8;
            return true;
        }
    }
    xkb_keysym_t sym = xkb_keysym_from_name(token, XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol) {
        sym = xkb_keysym_from_name(token, XKB_KEYSYM_CASE_INSENSITIVE);
    }
    if (sym == XKB_KEY_NoSymbol) {
        return false;
    }
    sym = xkb_keysym_to_lower(sym);
    // Digits and the keys without a printable symbol are positions: us's
    // key for that symbol (a hand-written "Return", "1" or "F5").
    if (!printable(sym) || (sym >= XKB_KEY_0 && sym <= XKB_KEY_9)) {
        out->kind = VELA_KEY_CODE;
        return us_keycode_for(sym, &out->key);
    }
    out->kind = VELA_KEY_SYM;
    out->key = sym;
    return true;
}

bool vela_combo_parse(const char* text, struct vela_combo* out)
{
    char copy[128];
    if (!text || strlen(text) >= sizeof(copy)) {
        return false;
    }
    memcpy(copy, text, strlen(text) + 1);
    struct vela_combo combo = { 0 };
    char* token = copy;
    for (;;) {
        char* plus = strchr(token, '+');
        if (!plus) {
            break;
        }
        *plus = '\0';
        bool known = false;
        for (size_t i = 0; i < sizeof(modifier_names) / sizeof(modifier_names[0]); ++i) {
            if (strcasecmp(token, modifier_names[i].name) == 0) {
                combo.mods |= modifier_names[i].mod;
                known = true;
                break;
            }
        }
        if (!known) {
            return false;
        }
        token = plus + 1;
    }
    if (!parse_key(token, &combo)) {
        return false;
    }
    *out = combo;
    return true;
}

bool vela_combo_format(const struct vela_combo* combo, char* out, size_t size)
{
    char key[64] = "";
    if (combo->kind == VELA_KEY_SYM) {
        if (xkb_keysym_get_name(combo->key, key, sizeof(key)) <= 0) {
            return false;
        }
    } else if (combo->kind == VELA_KEY_CODE) {
        struct xkb_keymap* us = vela_shortcuts_us_keymap();
        const char* name = us ? xkb_keymap_key_get_name(us, combo->key + 8) : NULL;
        if (!name || strlen(name) >= sizeof(key)) {
            return false;
        }
        memcpy(key, name, strlen(name) + 1);
    } else {
        return false;
    }
    for (const char* c = key; *c; ++c) {
        if (!isalnum((unsigned char)*c) && *c != '_') {
            return false;
        }
    }
    const int written = snprintf(out, size, "%s%s%s%s%s", combo->mods & VELA_MOD_CTRL ? "Ctrl+" : "",
        combo->mods & VELA_MOD_ALT ? "Alt+" : "", combo->mods & VELA_MOD_SHIFT ? "Shift+" : "",
        combo->mods & VELA_MOD_SUPER ? "Super+" : "", key);
    return written > 0 && (size_t)written < size;
}
