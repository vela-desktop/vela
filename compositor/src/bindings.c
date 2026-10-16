// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bindings.h"

#include "a11y.h"
#include "focus.h"
#include "input.h"
#include "interact.h"
#include "lock.h"
#include "output.h"
#include "process.h"
#include "server.h"
#include "shell.h"
#include "snap.h"
#include "switcher.h"
#include "view.h"
#include "workspace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <xkbcommon/xkbcommon.h>

// The shell's command, if there is a shell.
static bool shell(struct vela_server *server, const char *line)
{
    vela_shell_send(server, line);
    return true;
}

bool vela_bindings_repeats(uint32_t sym)
{
    return sym == XKB_KEY_XF86AudioRaiseVolume || sym == XKB_KEY_XF86AudioLowerVolume;
}

bool vela_bindings_handle(struct vela_server *server, uint32_t modifiers, uint32_t sym)
{
    bool alt = modifiers & WLR_MODIFIER_ALT;
    bool super = modifiers & WLR_MODIFIER_LOGO;
    bool shift = modifiers & WLR_MODIFIER_SHIFT;
    bool ctrl = modifiers & WLR_MODIFIER_CTRL;
    // The Alt+letter variants exist because, when Vela runs in a window inside
    // KDE, KDE keeps the Super key for itself. In the real session they are
    // left to the apps, which use them to open their own menus.
    bool alt_nested = alt && server->nested;
    bool windows_key = super || alt_nested;
    struct vela_a11y *a11y = server->a11y;

    // Ctrl+Alt+F1...F12: another console (or the Plasma session). The keyboard
    // already translates the combination into the XF86Switch_VT_n key.
    if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
        if (server->session) {
            wlr_session_change_vt(server->session, (unsigned)(sym - XKB_KEY_XF86Switch_VT_1 + 1));
        }
        return true;
    }
    // The keyboard's media and volume keys (and knob) work whatever window has
    // the focus, even on the lock screen, like Windows: the shell plays or
    // pauses the active player (MPRIS), changes the volume and mutes the
    // microphone. The app never sees them, or Play/Pause would toggle twice.
    static const struct {
        xkb_keysym_t key;
        const char *command;
    } media_keys[] = {
        { XKB_KEY_XF86AudioRaiseVolume, "volume up" },
        { XKB_KEY_XF86AudioLowerVolume, "volume down" },
        { XKB_KEY_XF86AudioMute, "volume mute" },
        { XKB_KEY_XF86AudioMicMute, "volume mic-mute" },
        { XKB_KEY_XF86AudioPlay, "media play-pause" },
        { XKB_KEY_XF86AudioPause, "media pause" },
        { XKB_KEY_XF86AudioStop, "media stop" },
        { XKB_KEY_XF86AudioNext, "media next" },
        { XKB_KEY_XF86AudioPrev, "media previous" },
    };
    for (size_t i = 0; i < sizeof(media_keys) / sizeof(media_keys[0]); ++i) {
        if (sym == media_keys[i].key) {
            return shell(server, media_keys[i].command);
        }
    }
    // The focused app keeps the shortcuts for itself (virtual machine, remote
    // desktop), or the screen is locked: only the keys above are left.
    if (vela_input_shortcuts_inhibited(server->input) || server->locked) {
        return false;
    }
    // Win+L locks the screen, like Windows (Alt+L inside KDE).
    if (windows_key && (sym == XKB_KEY_l || sym == XKB_KEY_L)) {
        vela_lock_screen(server->lock);
        return true;
    }
    if (alt && shift && sym == XKB_KEY_Escape) {
        wl_display_terminate(server->display);
        return true;
    }
    // The polkit dialog waits for an answer: the other shortcuts (Start,
    // Alt+Tab, Win+D, snap, desktops...) stay off, like Windows' secure
    // desktop. Locking the screen is still possible (above).
    if (vela_focus_modal_layer(server)) {
        return false;
    }
    if (windows_key && sym == XKB_KEY_Return) {
        const char *terminal = getenv("VELA_TERMINAL");
        vela_spawn(terminal ? terminal : "konsole || foot || kitty || alacritty || xterm");
        return true;
    }
    struct vela_view *active = vela_views_focused(server);
    if ((alt && sym == XKB_KEY_F4) || (alt_nested && sym == XKB_KEY_q)) {
        if (active) {
            vela_view_close(active);
        }
        return true;
    }
    // Alt+Tab (inside KDE: Alt+J), backwards with Shift. The choice goes on
    // while Alt is held; releasing it switches to the chosen window
    // (keyboard.c), Esc cancels.
    bool tab = sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab;
    bool nested_tab = sym == XKB_KEY_j || sym == XKB_KEY_J;
    if ((alt && tab) || (alt_nested && nested_tab)) {
        vela_switcher_step(server, shift || sym == XKB_KEY_ISO_Left_Tab ? -1 : 1);
        return true;
    }
    if (vela_switcher_active(server) && sym == XKB_KEY_Escape) {
        vela_switcher_finish(server, false);
        return true;
    }
    // Virtual desktops, like Windows: Win+Ctrl+←/→ to switch, Win+Ctrl+D to
    // create a new one, Win+Ctrl+F4 to close the current one (inside KDE with
    // Alt+Ctrl); Win+Tab (Alt+W) Task View.
    if (windows_key && ctrl) {
        if (sym == XKB_KEY_Left || sym == XKB_KEY_Right) {
            vela_workspaces_switch(server, server->workspaces->current + (sym == XKB_KEY_Left ? -1 : 1), true);
            return true;
        }
        if (sym == XKB_KEY_d || sym == XKB_KEY_D) {
            vela_workspaces_switch(server, vela_workspaces_add(server), true);
            return true;
        }
        if (sym == XKB_KEY_F4) {
            vela_workspaces_remove(server, server->workspaces->current);
            return true;
        }
    }
    // Accessibility: Win+Ctrl+C the color filters (if the shortcut is on in
    // Settings, like Windows); Win+plus and Win+minus the magnifier, Win+Esc
    // closes it.
    if (super && ctrl && xkb_keysym_to_lower(sym) == XKB_KEY_c && a11y->color_filter_shortcut) {
        vela_a11y_set_color_filter(a11y, !a11y->color_filter, true);
        return true;
    }
    if (super && (sym == XKB_KEY_plus || sym == XKB_KEY_equal || sym == XKB_KEY_KP_Add)) {
        vela_a11y_zoom(a11y, 1);
        return true;
    }
    if (super && a11y->magnifier && (sym == XKB_KEY_minus || sym == XKB_KEY_KP_Subtract)) {
        vela_a11y_zoom(a11y, -1);
        return true;
    }
    if (super && a11y->magnifier && sym == XKB_KEY_Escape) {
        vela_a11y_set_magnifier(a11y, false);
        return true;
    }
    // Win+V: clipboard history; Win+Ctrl+V: the sound outputs and the volume
    // mixer, in quick settings; Win+Shift+S and Print: the Snipping Tool
    // (drawn by the shell).
    if (super && xkb_keysym_to_lower(sym) == XKB_KEY_v) {
        return shell(server, ctrl ? "sound-output" : "clipboard");
    }
    if ((super && shift && xkb_keysym_to_lower(sym) == XKB_KEY_s) || sym == XKB_KEY_Print) {
        return shell(server, "snip");
    }
    if ((super && tab) || (alt_nested && (sym == XKB_KEY_w || sym == XKB_KEY_W))) {
        return shell(server, "task-view");
    }
    // Win+arrows: halves, quarters, maximize, minimize (snap.c). Inside KDE
    // Alt+arrows and Alt+M (maximize or restore).
    if (windows_key && (sym == XKB_KEY_Left || sym == XKB_KEY_Right || (super && (sym == XKB_KEY_Up || sym == XKB_KEY_Down)))) {
        if (active) {
            vela_snap_keyboard(server, active, sym);
        }
        return true;
    }
    if (alt_nested && sym == XKB_KEY_m) {
        if (active) {
            vela_view_set_maximized(active, !active->maximized, true);
        }
        return true;
    }
    // Win+Z: the active window's snap layouts.
    if (windows_key && (sym == XKB_KEY_z || sym == XKB_KEY_Z)) {
        if (active) {
            vela_snap_show_layouts(server, active, true);
        }
        return true;
    }
    if (alt_nested && sym == XKB_KEY_s) {
        return shell(server, "toggle-start");
    }
    // Win+X: the Start button menu; Win+R: Run; Win+D: the desktop; Win+A:
    // quick settings; Win+N: notification center and calendar; Win+I:
    // Settings; Win+E: File Explorer. Inside KDE with Alt.
    static const struct {
        xkb_keysym_t key;
        const char *command;
    } shell_keys[] = {
        { XKB_KEY_x, "winx" },
        { XKB_KEY_r, "run" },
        { XKB_KEY_d, "show-desktop" },
        { XKB_KEY_a, "quick-settings" },
        { XKB_KEY_n, "notification-center" },
        { XKB_KEY_i, "settings" },
        { XKB_KEY_e, "files" },
    };
    xkb_keysym_t lower = xkb_keysym_to_lower(sym);
    for (size_t i = 0; windows_key && i < sizeof(shell_keys) / sizeof(shell_keys[0]); ++i) {
        if (lower == shell_keys[i].key) {
            return shell(server, shell_keys[i].command);
        }
    }
    // Alt+Space: the window menu, under its title bar.
    if (alt && !super && sym == XKB_KEY_space && active) {
        struct wlr_box frame = vela_view_frame_box(active);
        int bar = vela_view_title_bar_height(active) > 0 ? vela_view_title_bar_height(active) : 36;
        vela_window_menu_show(server, active, frame.x + 4, frame.y + bar, true);
        return true;
    }
    return false;
}

void vela_window_menu_show(struct vela_server *server, struct vela_view *view, double lx, double ly, bool keyboard)
{
    if (!view || !view->ext_handle || server->locked) {
        return;
    }
    struct vela_output *output = vela_output_at(server, lx, ly);
    if (!output) {
        output = vela_view_output(view);
    }
    if (!output) {
        return;
    }
    // To the shell: the window, the output and the point in output
    // coordinates.
    struct wlr_box box = vela_output_box(output);
    char line[512];
    snprintf(line, sizeof(line), "window-menu %s %s %d %d %d %d %d %d", view->ext_handle->identifier,
        output->wlr->name, (int)lround(lx - box.x), (int)lround(ly - box.y), view->maximized ? 1 : 0,
        vela_view_resizable(view) ? 1 : 0, keyboard ? 1 : 0, view->always_on_top ? 1 : 0);
    vela_shell_send(server, line);
}

static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

void vela_window_action(struct vela_server *server, struct vela_view *view, const char *action)
{
    if (strcmp(action, "activate") == 0) {
        // From Task View: bring to front (on its desktop).
        if (view->minimized) {
            vela_view_set_minimized(view, false);
        } else {
            vela_focus_view(server, view);
        }
    } else if (strcmp(action, "restore") == 0) {
        if (view->minimized) {
            vela_view_set_minimized(view, false);
            vela_focus_view(server, view);
        } else if (view->maximized) {
            vela_view_set_maximized(view, false, true);
        } else if (!vela_snap_is_none(view->snap)) {
            vela_view_set_snap(view, vela_snap_none, NULL);
        }
    } else if (strcmp(action, "minimize") == 0) {
        vela_view_set_minimized(view, true);
    } else if (strcmp(action, "maximize") == 0) {
        vela_view_set_maximized(view, true, true);
    } else if (strcmp(action, "close") == 0) {
        vela_view_close(view);
    } else if (strncmp(action, "always-on-top", 13) == 0) {
        // "always-on-top [on|off|toggle]" (from the window menu: toggle).
        const char *arg = action[13] == ' ' ? action + 14 : "toggle";
        bool on = strcmp(arg, "toggle") == 0 ? !view->always_on_top : strcmp(arg, "on") == 0;
        vela_view_set_always_on_top(view, on);
    } else if (strcmp(action, "move") == 0) {
        vela_interact_begin_keyboard(server, view, VELA_CURSOR_MOVE);
    } else if (strcmp(action, "resize") == 0) {
        vela_interact_begin_keyboard(server, view, VELA_CURSOR_RESIZE);
    } else if (strcmp(action, "snap-left") == 0 || strcmp(action, "snap-right") == 0) {
        // From Task View: "Snap left/right".
        vela_focus_view(server, view);
        vela_view_set_snap(view, strcmp(action, "snap-left") == 0 ? vela_snap_left : vela_snap_right, NULL);
    } else if (starts_with(action, "snap ")) {
        // From snap layouts and Snap Assist: "snap x0 y0 x1 y1 [quiet
        // [window]]", in twelfths. From Snap Assist also the window it goes
        // next to: together they form a snap group.
        struct vela_snap tile = vela_snap_none;
        char quiet[8] = "";
        char origin[128] = "";
        int n = sscanf(action + 5, "%d %d %d %d %7s %127s", &tile.x0, &tile.y0, &tile.x1, &tile.y1, quiet, origin);
        if (n >= 4 && vela_snap_valid(tile)) {
            if (view->minimized) {
                vela_view_set_minimized(view, false);
            }
            vela_focus_view(server, view);
            vela_view_set_snap(view, tile, NULL);
            if (strcmp(quiet, "quiet") != 0) {
                vela_snap_offer_assist(server, view);
            } else if (n >= 6) {
                struct vela_view *other;
                wl_list_for_each (other, &server->views, link) {
                    if (other->ext_handle && strcmp(origin, other->ext_handle->identifier) == 0) {
                        vela_snap_join_group(server, view, other);
                    }
                }
            }
        }
    } else if (strcmp(action, "activate-group") == 0) {
        // The snap group from the taskbar: all its windows to the front.
        vela_snap_activate_group(server, view);
    } else if (starts_with(action, "move-to ")) {
        vela_workspaces_move_view(server, view, atoi(action + 8));
    } else if (strcmp(action, "move-to-new") == 0) {
        vela_workspaces_move_view(server, view, vela_workspaces_add(server));
    } else if (strcmp(action, "sticky") == 0 || strcmp(action, "unsticky") == 0) {
        vela_workspaces_set_sticky(server, view, strcmp(action, "sticky") == 0);
    } else if (strcmp(action, "app-sticky") == 0 || strcmp(action, "app-unsticky") == 0) {
        vela_workspaces_set_app_sticky(server, vela_view_app_id(view), strcmp(action, "app-sticky") == 0);
    }
}
