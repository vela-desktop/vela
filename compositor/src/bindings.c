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
#include "table.h"
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

// The modes the table's rows depend on, as they are now.
static struct vela_modes current_modes(struct vela_server *server)
{
    return (struct vela_modes) {
        .nested = server->nested,
        .color_filter_shortcut = server->a11y->color_filter_shortcut,
        .magnifier = server->a11y->magnifier,
        .switcher = vela_switcher_active(server),
        .numbers = server->input->super_numbers,
    };
}

static bool snap(struct vela_server *server, xkb_keysym_t direction)
{
    struct vela_view *active = vela_views_focused(server);
    if (active) {
        vela_snap_keyboard(server, active, direction);
    }
    return true;
}

// A row of the table (common/shortcuts/table.c): true if the key was used.
static bool run(struct vela_server *server, const struct vela_binding *row, const struct vela_press *press)
{
    struct vela_a11y *a11y = server->a11y;
    struct vela_view *active = vela_views_focused(server);
    switch (row->action) {
    case VELA_ACTION_SWITCH_VT:
        if (server->session) {
            wlr_session_change_vt(server->session, (unsigned)(press->typed - XKB_KEY_XF86Switch_VT_1 + 1));
        }
        return true;
    case VELA_ACTION_SHELL: return shell(server, row->arg);
    case VELA_ACTION_LOCK: vela_lock_screen(server->lock); return true;
    case VELA_ACTION_QUIT: wl_display_terminate(server->display); return true;
    case VELA_ACTION_TERMINAL: {
        const char *terminal = getenv("VELA_TERMINAL");
        vela_spawn(terminal ? terminal : "konsole || foot || kitty || alacritty || xterm");
        return true;
    }
    case VELA_ACTION_CLOSE_WINDOW:
        if (active) {
            vela_view_close(active);
        }
        return true;
    case VELA_ACTION_SWITCHER_NEXT: vela_switcher_step(server, 1); return true;
    case VELA_ACTION_SWITCHER_PREVIOUS: vela_switcher_step(server, -1); return true;
    case VELA_ACTION_SWITCHER_CANCEL: vela_switcher_finish(server, false); return true;
    case VELA_ACTION_DESKTOP_PREVIOUS:
    case VELA_ACTION_DESKTOP_NEXT:
        vela_workspaces_switch(
            server, server->workspaces->current + (row->action == VELA_ACTION_DESKTOP_PREVIOUS ? -1 : 1), true);
        return true;
    case VELA_ACTION_DESKTOP_NEW: vela_workspaces_switch(server, vela_workspaces_add(server), true); return true;
    case VELA_ACTION_DESKTOP_CLOSE: vela_workspaces_remove(server, server->workspaces->current); return true;
    case VELA_ACTION_COLOR_FILTERS: vela_a11y_set_color_filter(a11y, !a11y->color_filter, true); return true;
    case VELA_ACTION_ZOOM_IN: vela_a11y_zoom(a11y, 1); return true;
    case VELA_ACTION_ZOOM_OUT: vela_a11y_zoom(a11y, -1); return true;
    case VELA_ACTION_MAGNIFIER_OFF: vela_a11y_set_magnifier(a11y, false); return true;
    case VELA_ACTION_SNAP_LEFT: return snap(server, XKB_KEY_Left);
    case VELA_ACTION_SNAP_RIGHT: return snap(server, XKB_KEY_Right);
    case VELA_ACTION_SNAP_UP: return snap(server, XKB_KEY_Up);
    case VELA_ACTION_SNAP_DOWN: return snap(server, XKB_KEY_Down);
    case VELA_ACTION_MAXIMIZE:
        if (active) {
            vela_view_set_maximized(active, !active->maximized, true);
        }
        return true;
    case VELA_ACTION_SNAP_LAYOUTS:
        if (active) {
            vela_snap_show_layouts(server, active, true);
        }
        return true;
    case VELA_ACTION_WINDOW_MENU: {
        // Without a window the app keeps Alt+Space.
        if (!active) {
            return false;
        }
        struct wlr_box frame = vela_view_frame_box(active);
        int bar = vela_view_title_bar_height(active) > 0 ? vela_view_title_bar_height(active) : 36;
        vela_window_menu_show(server, active, frame.x + 4, frame.y + bar, true);
        return true;
    }
    // Run elsewhere: the key goes on as it would.
    case VELA_ACTION_TASKBAR_NUMBER:
    case VELA_ACTION_NEXT_LAYOUT: return false;
    }
    return false;
}

bool vela_bindings_handle(struct vela_server *server, const struct vela_press *press, bool *repeats)
{
    *repeats = false;
    const struct vela_modes modes = current_modes(server);
    const struct vela_binding *row = vela_binding_find(press, &modes, VELA_STAGE_ALWAYS);
    // The focused app keeps the shortcuts for itself (virtual machine, remote
    // desktop), or the screen is locked: only the keys above are left.
    if (!row && !vela_input_shortcuts_inhibited(server->input) && !server->locked) {
        row = vela_binding_find(press, &modes, VELA_STAGE_BEFORE_DIALOG);
        // The polkit dialog waits for an answer: the other shortcuts (Start,
        // Alt+Tab, Win+D, snap, desktops...) stay off, like Windows' secure
        // desktop. Locking the screen is still possible (above).
        if (!row && !vela_focus_modal_layer(server)) {
            row = vela_binding_find(press, &modes, VELA_STAGE_NORMAL);
        }
    }
    if (!row) {
        return false;
    }
    *repeats = row->repeats;
    return run(server, row, press);
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
    snprintf(line, sizeof(line), "window-menu %s %s %d %d %d %d %d", view->ext_handle->identifier,
        output->wlr->name, (int)lround(lx - box.x), (int)lround(ly - box.y), view->maximized ? 1 : 0,
        vela_view_resizable(view) ? 1 : 0, keyboard ? 1 : 0);
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
            vela_view_set_snap(view, vela_snap_none, NULL, true);
        }
    } else if (strcmp(action, "minimize") == 0) {
        vela_view_set_minimized(view, true);
    } else if (strcmp(action, "maximize") == 0) {
        vela_view_set_maximized(view, true, true);
    } else if (strcmp(action, "close") == 0) {
        vela_view_close(view);
    } else if (strcmp(action, "move") == 0) {
        vela_interact_begin_keyboard(server, view, VELA_CURSOR_MOVE);
    } else if (strcmp(action, "resize") == 0) {
        vela_interact_begin_keyboard(server, view, VELA_CURSOR_RESIZE);
    } else if (strcmp(action, "snap-left") == 0 || strcmp(action, "snap-right") == 0) {
        // From Task View: "Snap left/right".
        vela_focus_view(server, view);
        vela_view_set_snap(view, strcmp(action, "snap-left") == 0 ? vela_snap_left : vela_snap_right, NULL, true);
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
            vela_view_set_snap(view, tile, NULL, true);
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
