// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_KEYMAP_OPTIONS_H
#define VELA_KEYMAP_OPTIONS_H

// The XKB options a keyboard gets (docs/keyboard.md §6): Vela's
// keyboard-options when vela.conf has the key, even empty, since Settings
// writes every option there, the system's (systemd-localed) otherwise; with
// several layouts and no layout-switch option, Win+Space switches.

#include <stdbool.h>
#include <stddef.h>

// `layout`: the layouts in use ("it,us"); `system`: localed's options;
// `vela`: vela.conf's keyboard-options, NULL when the key is absent. false
// if `out` (of `size`) is too small, with `out` empty.
bool vela_keymap_options(const char *layout, const char *system, const char *vela, char *out, size_t size);

#endif
