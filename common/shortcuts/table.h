// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SHORTCUT_TABLE_H
#define VELA_SHORTCUT_TABLE_H

// Vela's own shortcuts as one table (docs/keyboard.md §9.2,
// docs/shortcuts.md): the compositor runs it (compositor/src/bindings.c),
// Settings asks it whether a combination is taken. A row is a combination,
// the modes it needs and its action; the first row that matches wins.

#include "shortcuts.h"
#include "super_numbers.h"

enum vela_action {
    VELA_ACTION_SWITCH_VT, // Ctrl+Alt+F1…F12: arg is unused, the VT is the key's
    VELA_ACTION_SHELL, // a line for the shell (arg)
    VELA_ACTION_LOCK,
    VELA_ACTION_QUIT,
    VELA_ACTION_TERMINAL,
    VELA_ACTION_CLOSE_WINDOW,
    VELA_ACTION_SWITCHER_NEXT,
    VELA_ACTION_SWITCHER_PREVIOUS,
    VELA_ACTION_SWITCHER_CANCEL,
    VELA_ACTION_DESKTOP_PREVIOUS,
    VELA_ACTION_DESKTOP_NEXT,
    VELA_ACTION_DESKTOP_NEW,
    VELA_ACTION_DESKTOP_CLOSE,
    VELA_ACTION_COLOR_FILTERS,
    VELA_ACTION_ZOOM_IN,
    VELA_ACTION_ZOOM_OUT,
    VELA_ACTION_MAGNIFIER_OFF,
    VELA_ACTION_SNAP_LEFT,
    VELA_ACTION_SNAP_RIGHT,
    VELA_ACTION_SNAP_UP, // maximize, or restore
    VELA_ACTION_SNAP_DOWN, // restore, then minimize
    VELA_ACTION_MAXIMIZE, // maximize or restore (nested)
    VELA_ACTION_SNAP_LAYOUTS,
    VELA_ACTION_WINDOW_MENU,
    // Run elsewhere: listed so that Settings knows they are taken.
    VELA_ACTION_TASKBAR_NUMBER, // Win+number (keyboard.c, super_numbers.h)
    VELA_ACTION_NEXT_LAYOUT, // Win+Space: XKB's grp:win_space_toggle
};

// When a row applies.
enum {
    VELA_WHEN_ANY_MODS = 1 << 0, // whatever the modifiers (media keys)
    VELA_WHEN_NESTED = 1 << 1, // Vela in a window of another desktop
    VELA_WHEN_COLOR_FILTER_SHORTCUT = 1 << 2, // color-filters-shortcut=yes
    VELA_WHEN_MAGNIFIER = 1 << 3, // the magnifier is on
    VELA_WHEN_SWITCHER = 1 << 4, // Alt+Tab is choosing
    VELA_WHEN_NUMBERS_APPS = 1 << 5, // super-number-keys=apps
    VELA_WHEN_NUMBERS_DESKTOPS = 1 << 6, // super-number-keys=desktops
};

// The order the compositor checks them in.
enum vela_stage {
    // Even on the lock screen and while an app inhibits shortcuts.
    VELA_STAGE_ALWAYS,
    // Not while inhibited or locked, but while the polkit dialog waits.
    VELA_STAGE_BEFORE_DIALOG,
    // The rest: not while the polkit dialog waits either.
    VELA_STAGE_NORMAL,
};

struct vela_binding {
    enum vela_stage stage;
    uint8_t mods;
    enum vela_key_kind kind;
    uint32_t key;
    uint8_t span; // keys key … key + span - 1 (the VTs, the digit row); 0 is 1
    uint16_t when;
    enum vela_action action;
    const char *arg;
    bool repeats; // repeats while held (the volume keys)
};

struct vela_modes {
    bool nested;
    bool color_filter_shortcut;
    bool magnifier;
    bool switcher;
    enum vela_super_numbers numbers;
};

// The first row of `stage` that `press` matches in `modes`, or NULL.
const struct vela_binding *vela_binding_find(
    const struct vela_press *press, const struct vela_modes *modes, enum vela_stage stage);

// The first row of any stage, or NULL: whether Vela takes the combination.
const struct vela_binding *vela_binding_taken(const struct vela_press *press, const struct vela_modes *modes);

// The rows, for the tests.
const struct vela_binding *vela_bindings_table(size_t *count);

#endif
