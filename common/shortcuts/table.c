// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "table.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#define CTRL VELA_MOD_CTRL
#define ALT VELA_MOD_ALT
#define SHIFT VELA_MOD_SHIFT
#define SUPER VELA_MOD_SUPER
#define NESTED VELA_WHEN_NESTED

// A letter or another printable key, by its symbol.
#define SYM(stage, mods, sym, when, action, arg) { stage, mods, VELA_KEY_SYM, sym, 1, when, action, arg, false }
// A key by its position.
#define CODE(stage, mods, code, when, action, arg) { stage, mods, VELA_KEY_CODE, code, 1, when, action, arg, false }
#define ALWAYS VELA_STAGE_ALWAYS
#define DIALOG VELA_STAGE_BEFORE_DIALOG
#define NORMAL VELA_STAGE_NORMAL

// The Alt+letter variants exist because, when Vela runs in a window inside
// another desktop, that desktop keeps the Super key for itself. In the real
// session they are left to the apps, which use them to open their menus.
static const struct vela_binding table[] = {
    // Ctrl+Alt+F1…F12: another console. The keyboard already turns the
    // combination into XF86Switch_VT_n.
    { ALWAYS, 0, VELA_KEY_TYPED, XKB_KEY_XF86Switch_VT_1, 12, VELA_WHEN_ANY_MODS, VELA_ACTION_SWITCH_VT, NULL, false },
    // The media and volume keys (and knob), whatever has the focus, even on
    // the lock screen: the shell plays or pauses the active player (MPRIS),
    // changes the volume and mutes the microphone. The app never sees them,
    // or Play/Pause would toggle twice. The volume repeats while held.
    { ALWAYS, 0, VELA_KEY_SYM, XKB_KEY_XF86AudioRaiseVolume, 1, VELA_WHEN_ANY_MODS, VELA_ACTION_SHELL, "volume up",
        true },
    { ALWAYS, 0, VELA_KEY_SYM, XKB_KEY_XF86AudioLowerVolume, 1, VELA_WHEN_ANY_MODS, VELA_ACTION_SHELL, "volume down",
        true },
    SYM(ALWAYS, 0, XKB_KEY_XF86AudioMute, VELA_WHEN_ANY_MODS, VELA_ACTION_SHELL, "volume mute"),
    SYM(ALWAYS, 0, XKB_KEY_XF86AudioMicMute, VELA_WHEN_ANY_MODS, VELA_ACTION_SHELL, "volume mic-mute"),
    SYM(ALWAYS, 0, XKB_KEY_XF86AudioPlay, VELA_WHEN_ANY_MODS, VELA_ACTION_SHELL, "media play-pause"),
    SYM(ALWAYS, 0, XKB_KEY_XF86AudioPause, VELA_WHEN_ANY_MODS, VELA_ACTION_SHELL, "media pause"),
    SYM(ALWAYS, 0, XKB_KEY_XF86AudioStop, VELA_WHEN_ANY_MODS, VELA_ACTION_SHELL, "media stop"),
    SYM(ALWAYS, 0, XKB_KEY_XF86AudioNext, VELA_WHEN_ANY_MODS, VELA_ACTION_SHELL, "media next"),
    SYM(ALWAYS, 0, XKB_KEY_XF86AudioPrev, VELA_WHEN_ANY_MODS, VELA_ACTION_SHELL, "media previous"),

    // Win+L locks the screen; Alt+Shift+Esc quits Vela. Still there while
    // the polkit dialog waits.
    SYM(DIALOG, SUPER, XKB_KEY_l, 0, VELA_ACTION_LOCK, NULL),
    SYM(DIALOG, ALT, XKB_KEY_l, NESTED, VELA_ACTION_LOCK, NULL),
    CODE(DIALOG, ALT | SHIFT, KEY_ESC, 0, VELA_ACTION_QUIT, NULL),

    CODE(NORMAL, SUPER, KEY_ENTER, 0, VELA_ACTION_TERMINAL, NULL),
    CODE(NORMAL, ALT, KEY_ENTER, NESTED, VELA_ACTION_TERMINAL, NULL),
    CODE(NORMAL, ALT, KEY_F4, 0, VELA_ACTION_CLOSE_WINDOW, NULL),
    SYM(NORMAL, ALT, XKB_KEY_q, NESTED, VELA_ACTION_CLOSE_WINDOW, NULL),
    // Alt+Tab, backwards with Shift. The choice goes on while Alt is held;
    // releasing it switches to the chosen window (keyboard.c), Esc cancels.
    CODE(NORMAL, ALT, KEY_TAB, 0, VELA_ACTION_SWITCHER_NEXT, NULL),
    CODE(NORMAL, ALT | SHIFT, KEY_TAB, 0, VELA_ACTION_SWITCHER_PREVIOUS, NULL),
    SYM(NORMAL, ALT, XKB_KEY_j, NESTED, VELA_ACTION_SWITCHER_NEXT, NULL),
    SYM(NORMAL, ALT | SHIFT, XKB_KEY_j, NESTED, VELA_ACTION_SWITCHER_PREVIOUS, NULL),
    CODE(NORMAL, 0, KEY_ESC, VELA_WHEN_SWITCHER | VELA_WHEN_ANY_MODS, VELA_ACTION_SWITCHER_CANCEL, NULL),
    // Virtual desktops: Win+Ctrl+←/→ to switch, Win+Ctrl+D a new one,
    // Win+Ctrl+F4 closes the current one. Nested, not Alt+Ctrl+F4: that is
    // the fourth console.
    CODE(NORMAL, SUPER | CTRL, KEY_LEFT, 0, VELA_ACTION_DESKTOP_PREVIOUS, NULL),
    CODE(NORMAL, SUPER | CTRL, KEY_RIGHT, 0, VELA_ACTION_DESKTOP_NEXT, NULL),
    SYM(NORMAL, SUPER | CTRL, XKB_KEY_d, 0, VELA_ACTION_DESKTOP_NEW, NULL),
    CODE(NORMAL, SUPER | CTRL, KEY_F4, 0, VELA_ACTION_DESKTOP_CLOSE, NULL),
    CODE(NORMAL, ALT | CTRL, KEY_LEFT, NESTED, VELA_ACTION_DESKTOP_PREVIOUS, NULL),
    CODE(NORMAL, ALT | CTRL, KEY_RIGHT, NESTED, VELA_ACTION_DESKTOP_NEXT, NULL),
    SYM(NORMAL, ALT | CTRL, XKB_KEY_d, NESTED, VELA_ACTION_DESKTOP_NEW, NULL),
    // Accessibility: Win+Ctrl+C the color filters (if the shortcut is on in
    // Settings); Win+plus and Win+minus the magnifier, Win+Esc closes it.
    SYM(NORMAL, SUPER | CTRL, XKB_KEY_c, VELA_WHEN_COLOR_FILTER_SHORTCUT, VELA_ACTION_COLOR_FILTERS, NULL),
    SYM(NORMAL, SUPER, XKB_KEY_plus, 0, VELA_ACTION_ZOOM_IN, NULL),
    SYM(NORMAL, SUPER, XKB_KEY_equal, 0, VELA_ACTION_ZOOM_IN, NULL), // the + key of us
    SYM(NORMAL, SUPER | SHIFT, XKB_KEY_equal, 0, VELA_ACTION_ZOOM_IN, NULL), // ...where + takes Shift
    CODE(NORMAL, SUPER, KEY_KPPLUS, 0, VELA_ACTION_ZOOM_IN, NULL),
    SYM(NORMAL, SUPER, XKB_KEY_minus, VELA_WHEN_MAGNIFIER, VELA_ACTION_ZOOM_OUT, NULL),
    CODE(NORMAL, SUPER, KEY_KPMINUS, VELA_WHEN_MAGNIFIER, VELA_ACTION_ZOOM_OUT, NULL),
    CODE(NORMAL, SUPER, KEY_ESC, VELA_WHEN_MAGNIFIER, VELA_ACTION_MAGNIFIER_OFF, NULL),
    // Win+V the clipboard history; Win+Ctrl+V the sound outputs and the
    // mixer, in quick settings; Win+Shift+S and Print the Snipping Tool.
    SYM(NORMAL, SUPER, XKB_KEY_v, 0, VELA_ACTION_SHELL, "clipboard"),
    SYM(NORMAL, SUPER | CTRL, XKB_KEY_v, 0, VELA_ACTION_SHELL, "sound-output"),
    SYM(NORMAL, SUPER | SHIFT, XKB_KEY_s, 0, VELA_ACTION_SHELL, "snip"),
    CODE(NORMAL, 0, KEY_SYSRQ, 0, VELA_ACTION_SHELL, "snip"), // Print
    CODE(NORMAL, SUPER, KEY_TAB, 0, VELA_ACTION_SHELL, "task-view"),
    SYM(NORMAL, ALT, XKB_KEY_w, NESTED, VELA_ACTION_SHELL, "task-view"),
    // Win+arrows: halves, quarters, maximize, minimize (snap.c).
    CODE(NORMAL, SUPER, KEY_LEFT, 0, VELA_ACTION_SNAP_LEFT, NULL),
    CODE(NORMAL, SUPER, KEY_RIGHT, 0, VELA_ACTION_SNAP_RIGHT, NULL),
    CODE(NORMAL, SUPER, KEY_UP, 0, VELA_ACTION_SNAP_UP, NULL),
    CODE(NORMAL, SUPER, KEY_DOWN, 0, VELA_ACTION_SNAP_DOWN, NULL),
    CODE(NORMAL, ALT, KEY_LEFT, NESTED, VELA_ACTION_SNAP_LEFT, NULL),
    CODE(NORMAL, ALT, KEY_RIGHT, NESTED, VELA_ACTION_SNAP_RIGHT, NULL),
    SYM(NORMAL, ALT, XKB_KEY_m, NESTED, VELA_ACTION_MAXIMIZE, NULL),
    // Win+Z: the active window's snap layouts.
    SYM(NORMAL, SUPER, XKB_KEY_z, 0, VELA_ACTION_SNAP_LAYOUTS, NULL),
    SYM(NORMAL, ALT, XKB_KEY_z, NESTED, VELA_ACTION_SNAP_LAYOUTS, NULL),
    SYM(NORMAL, ALT, XKB_KEY_s, NESTED, VELA_ACTION_SHELL, "toggle-start"),
    // Win+X the Start button's menu, Win+D the desktop, Win+A quick
    // settings, Win+N the notification center, Win+I Settings, Win+E Files.
    SYM(NORMAL, SUPER, XKB_KEY_x, 0, VELA_ACTION_SHELL, "winx"),
    SYM(NORMAL, SUPER, XKB_KEY_d, 0, VELA_ACTION_SHELL, "show-desktop"),
    SYM(NORMAL, SUPER, XKB_KEY_a, 0, VELA_ACTION_SHELL, "quick-settings"),
    SYM(NORMAL, SUPER, XKB_KEY_n, 0, VELA_ACTION_SHELL, "notification-center"),
    SYM(NORMAL, SUPER, XKB_KEY_i, 0, VELA_ACTION_SHELL, "settings"),
    SYM(NORMAL, SUPER, XKB_KEY_e, 0, VELA_ACTION_SHELL, "files"),
    SYM(NORMAL, ALT, XKB_KEY_x, NESTED, VELA_ACTION_SHELL, "winx"),
    SYM(NORMAL, ALT, XKB_KEY_d, NESTED, VELA_ACTION_SHELL, "show-desktop"),
    SYM(NORMAL, ALT, XKB_KEY_a, NESTED, VELA_ACTION_SHELL, "quick-settings"),
    SYM(NORMAL, ALT, XKB_KEY_n, NESTED, VELA_ACTION_SHELL, "notification-center"),
    SYM(NORMAL, ALT, XKB_KEY_i, NESTED, VELA_ACTION_SHELL, "settings"),
    SYM(NORMAL, ALT, XKB_KEY_e, NESTED, VELA_ACTION_SHELL, "files"),
    // Alt+Space: the window menu, under its title bar.
    CODE(NORMAL, ALT, KEY_SPACE, 0, VELA_ACTION_WINDOW_MENU, NULL),
    // Win+number (keyboard.c runs them before the table, for their
    // releases): the taskbar's apps, or the desktops (super-number-keys=).
    { NORMAL, SUPER, VELA_KEY_CODE, KEY_1, 10, VELA_WHEN_NUMBERS_APPS, VELA_ACTION_TASKBAR_NUMBER, "activate", false },
    { NORMAL, SUPER | SHIFT, VELA_KEY_CODE, KEY_1, 10, VELA_WHEN_NUMBERS_APPS, VELA_ACTION_TASKBAR_NUMBER, "new",
        false },
    { NORMAL, SUPER | ALT, VELA_KEY_CODE, KEY_1, 10, VELA_WHEN_NUMBERS_APPS, VELA_ACTION_TASKBAR_NUMBER, "jump",
        false },
    { NORMAL, SUPER, VELA_KEY_CODE, KEY_1, 10, VELA_WHEN_NUMBERS_DESKTOPS, VELA_ACTION_TASKBAR_NUMBER, "switch",
        false },
    { NORMAL, SUPER | SHIFT, VELA_KEY_CODE, KEY_1, 10, VELA_WHEN_NUMBERS_DESKTOPS, VELA_ACTION_TASKBAR_NUMBER, "move",
        false },
    // Win+Space: the next layout, by XKB itself when there are several. Its
    // key type looks at Win alone, so Win+Shift+Space switches too.
    CODE(NORMAL, SUPER, KEY_SPACE, VELA_WHEN_ANY_MODS, VELA_ACTION_NEXT_LAYOUT, NULL),
};

const struct vela_binding *vela_bindings_table(size_t *count)
{
    *count = sizeof(table) / sizeof(table[0]);
    return table;
}

static bool applies(const struct vela_binding *row, const struct vela_modes *modes)
{
    return (!(row->when & VELA_WHEN_NESTED) || modes->nested)
        && (!(row->when & VELA_WHEN_COLOR_FILTER_SHORTCUT) || modes->color_filter_shortcut)
        && (!(row->when & VELA_WHEN_MAGNIFIER) || modes->magnifier)
        && (!(row->when & VELA_WHEN_SWITCHER) || modes->switcher)
        && (!(row->when & VELA_WHEN_NUMBERS_APPS) || modes->numbers == VELA_SUPER_NUMBERS_APPS)
        && (!(row->when & VELA_WHEN_NUMBERS_DESKTOPS) || modes->numbers == VELA_SUPER_NUMBERS_DESKTOPS);
}

static bool matches(const struct vela_binding *row, const struct vela_press *press)
{
    if ((row->when & VELA_WHEN_ANY_MODS) ? (press->mods & row->mods) != row->mods : row->mods != press->mods) {
        return false;
    }
    const uint32_t key = row->kind == VELA_KEY_SYM ? press->sym
        : row->kind == VELA_KEY_CODE               ? press->keycode
                                                   : press->typed;
    const uint32_t span = row->span ? row->span : 1;
    return key >= row->key && key - row->key < span;
}

const struct vela_binding *vela_binding_find(
    const struct vela_press *press, const struct vela_modes *modes, enum vela_stage stage)
{
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (table[i].stage == stage && applies(&table[i], modes) && matches(&table[i], press)) {
            return &table[i];
        }
    }
    return NULL;
}

const struct vela_binding *vela_binding_taken(const struct vela_press *press, const struct vela_modes *modes)
{
    for (int stage = VELA_STAGE_ALWAYS; stage <= VELA_STAGE_NORMAL; ++stage) {
        const struct vela_binding *row = vela_binding_find(press, modes, (enum vela_stage)stage);
        if (row) {
            return row;
        }
    }
    return NULL;
}
