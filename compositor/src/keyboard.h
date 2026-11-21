// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_KEYBOARD_H
#define VELA_KEYBOARD_H

// A keyboard: its layout, key repeat and the path of every key (sticky keys,
// Super alone for Start, shortcuts, then the focused app).
//
// The layout, in order: the XKB_DEFAULT_* variables (field by field), the
// choice made in Vela's Settings (vela.conf), systemd-localed's (localectl,
// /etc/X11/xorg.conf.d/00-keyboard.conf), then "us" (docs/keyboard.md §5.6).
// A virtual keyboard (automated tests) brings its own.

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct vela_config;
struct vela_input;
struct wlr_keyboard;

struct vela_keyboard {
    struct wl_list link; // vela_input.keyboards
    struct vela_input *input;
    struct wlr_keyboard *wlr;
    uint16_t handled_numbers;
    bool restoring; // sticky keys: the modifiers are being put back
    // Caps Lock and Num Lock are being set from the seat's (§8.1): the
    // change is not news to share again.
    bool syncing;
    // A virtual keyboard (tests, remote desktop): it sends its own
    // modifiers and doesn't share the locks.
    bool is_virtual;
    // A virtual keyboard's own Caps Lock, last seen: when it changes, the
    // Caps Lock display shows it (§8.3).
    bool virtual_caps;
    // A shortcut held down that repeats (the volume keys): the app doesn't get
    // the key, so it can't repeat it. With the keyboard's delay and rate.
    struct wl_event_source *repeat;
    uint32_t repeat_keycode; // 0: nothing repeating
    uint32_t repeat_sym;
    uint32_t repeat_mods;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

// The keyboard becomes the seat's.
struct vela_keyboard *vela_keyboard_create(struct vela_input *input, struct wlr_keyboard *wlr);
void vela_keyboard_destroy(struct vela_keyboard *keyboard);

// Layout and key repeat from vela.conf.
void vela_keyboard_apply_settings(struct vela_keyboard *keyboard, const struct vela_config *settings);

// Keeps Num Lock's state now if a change is waiting (numlock=last): at exit.
void vela_keyboard_save_numlock(struct vela_input *input);

#endif
