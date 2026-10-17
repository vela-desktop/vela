// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "switcher.h"

#include "buffer.h"
#include "focus.h"
#include "server.h"
#include "shell.h"
#include "util.h"
#include "view.h"
#include "workspace.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>

struct vela_switcher *vela_switcher_create(void)
{
    return calloc(1, sizeof(struct vela_switcher));
}

void vela_switcher_destroy(struct vela_switcher *switcher)
{
    if (switcher) {
        free(switcher->views);
        free(switcher);
    }
}

bool vela_switcher_active(const struct vela_server *server)
{
    return server->switcher->active;
}

// "switcher-show N id1 id2..." or "switcher-select N": the shell finds the
// windows by identifier (ext-foreign-toplevel-list).
static void announce(struct vela_server *server, const char *command)
{
    struct vela_switcher *switcher = server->switcher;
    struct vela_buffer line = { 0 };
    vela_buffer_appendf(&line, "switcher-%s %d", command, switcher->selected);
    if (strcmp(command, "show") == 0) {
        for (int i = 0; i < switcher->count; ++i) {
            vela_buffer_appendf(&line, " %s", switcher->views[i]->ext_handle->identifier);
        }
    }
    vela_shell_send(server, vela_buffer_text(&line));
    vela_buffer_finish(&line);
}

void vela_switcher_step(struct vela_server *server, int direction)
{
    struct vela_switcher *switcher = server->switcher;
    if (switcher->active) {
        switcher->selected = (switcher->selected + switcher->count + direction) % switcher->count;
        announce(server, "select");
        return;
    }
    // All windows, most recent first; minimized ones are already at the end of
    // the list and are restored if chosen. Only the current desktop, like
    // Windows.
    switcher->count = 0;
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->mapped && view->ext_handle && vela_view_on_current_workspace(view) && !vela_view_stays_below(view)) {
            switcher->views = vela_grow(switcher->views, &switcher->capacity, switcher->count + 1,
                sizeof(*switcher->views));
            switcher->views[switcher->count++] = view;
        }
    }
    if (switcher->count == 0) {
        return;
    }
    switcher->active = true;
    // Start from the window used before the active one (or the most recent, if
    // none is active).
    bool front_active = vela_views_focused(server) == switcher->views[0];
    if (direction > 0) {
        switcher->selected = front_active ? 1 % switcher->count : 0;
    } else {
        switcher->selected = switcher->count - 1;
    }
    announce(server, "show");
}

void vela_switcher_finish(struct vela_server *server, bool activate)
{
    struct vela_switcher *switcher = server->switcher;
    if (!switcher->active) {
        return;
    }
    switcher->active = false;
    struct vela_view *chosen
        = activate && switcher->selected < switcher->count ? switcher->views[switcher->selected] : NULL;
    switcher->count = 0;
    vela_shell_send(server, "switcher-hide");
    if (chosen) {
        vela_focus_view(server, chosen);
    }
}

void vela_switcher_pick(struct vela_server *server, int index)
{
    struct vela_switcher *switcher = server->switcher;
    if (switcher->active && index >= 0 && index < switcher->count) {
        switcher->selected = index;
        vela_switcher_finish(server, true);
    }
}

void vela_switcher_forget(struct vela_server *server, struct vela_view *view)
{
    struct vela_switcher *switcher = server->switcher;
    for (int i = 0; i < switcher->count; ++i) {
        if (switcher->views[i] == view) {
            vela_switcher_finish(server, false);
            return;
        }
    }
}
