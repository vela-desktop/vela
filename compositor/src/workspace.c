// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "workspace.h"

#include "focus.h"
#include "buffer.h"
#include "config.h"
#include "motion.h"
#include "output.h"
#include "scene/scene.h"
#include "server.h"
#include "shell.h"
#include "snap.h"
#include "util.h"
#include "view.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

static void finish_switch(struct vela_server *server);

static void push_string(char ***items, int *count, int *capacity, const char *text)
{
    *items = vela_grow(*items, capacity, *count + 1, sizeof(**items));
    (*items)[(*count)++] = strdup(text);
}

static void free_strings(char **items, int count)
{
    for (int i = 0; i < count; ++i) {
        free(items[i]);
    }
    free(items);
}

struct vela_workspaces *vela_workspaces_create(struct vela_server *server)
{
    struct vela_workspaces *ws = calloc(1, sizeof(*ws));
    ws->server = server;
    ws->next_map_serial = 1;
    // desktop=<name> for each desktop (empty: "Desktop N");
    // sticky-app=<app_id>.
    char path[PATH_MAX];
    FILE *in = vela_config_path("desktop.conf", path, sizeof(path)) ? fopen(path, "re") : NULL;
    if (in) {
        char *line = NULL;
        size_t size = 0;
        ssize_t length;
        while ((length = getline(&line, &size, in)) >= 0) {
            if (length > 0 && line[length - 1] == '\n') {
                line[--length] = '\0';
            }
            if (strncmp(line, "desktop=", 8) == 0) {
                push_string(&ws->names, &ws->count, &ws->capacity, line + 8);
            } else if (strncmp(line, "sticky-app=", 11) == 0 && length > 11) {
                push_string(&ws->sticky_apps, &ws->sticky_count, &ws->sticky_capacity, line + 11);
            } else if (strncmp(line, "app-ovunque=", 12) == 0 && length > 12) {
                push_string(&ws->sticky_apps, &ws->sticky_count, &ws->sticky_capacity, line + 12); // the old name
            }
        }
        free(line);
        fclose(in);
    }
    if (ws->count == 0) {
        push_string(&ws->names, &ws->count, &ws->capacity, "");
    }
    return ws;
}

void vela_workspaces_destroy(struct vela_workspaces *ws)
{
    if (!ws) {
        return;
    }
    free_strings(ws->names, ws->count);
    free_strings(ws->sticky_apps, ws->sticky_count);
    free(ws->outgoing);
    free(ws);
}

static void save(const struct vela_workspaces *ws)
{
    char directory[PATH_MAX];
    char path[PATH_MAX];
    char temporary[PATH_MAX + 8];
    if (!vela_config_path(NULL, directory, sizeof(directory)) || !vela_config_path("desktop.conf", path, sizeof(path))
        || !vela_format(temporary, sizeof(temporary), "%s.XXXXXX", path)) {
        return;
    }
    mkdir(directory, 0755);
    int fd = mkstemp(temporary); // unique, exclusively created with mode 0600
    if (fd < 0) {
        return;
    }
    FILE *out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(temporary);
        return;
    }
    bool ok = fputs("# Vela's virtual desktops, in order (empty: \"Desktop N\")\n", out) >= 0;
    for (int i = 0; i < ws->count; ++i) {
        ok = fprintf(out, "desktop=%s\n", ws->names[i]) >= 0 && ok;
    }
    for (int i = 0; i < ws->sticky_count; ++i) {
        ok = fprintf(out, "sticky-app=%s\n", ws->sticky_apps[i]) >= 0 && ok;
    }
    ok = fclose(out) == 0 && ok;
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
    }
}

// The name to show ("Desktop N" if it has none).
static void workspace_name(const struct vela_workspaces *ws, int index, char *out, size_t size)
{
    if (index < 0 || index >= ws->count) {
        snprintf(out, size, "%s", "");
    } else if (!ws->names[index][0]) {
        snprintf(out, size, "Desktop %d", index + 1);
    } else {
        snprintf(out, size, "%s", ws->names[index]);
    }
}

bool vela_view_on_current_workspace(const struct vela_view *view)
{
    return view->sticky || view->workspace == view->server->workspaces->current;
}

// ----------------------------------------------------------- the shell --

static const char *view_id(const struct vela_view *view)
{
    return view->ext_handle ? view->ext_handle->identifier : NULL;
}

void vela_workspaces_json(struct vela_server *server, struct vela_buffer *out)
{
    // {"current":0,"names":["Desktop 1"],"windows":{"<ext id>":0 (-1:
    // all)},"stickyApps":[...]}
    const struct vela_workspaces *ws = server->workspaces;
    vela_buffer_appendf(out, "{\"current\":%d,\"names\":[", ws->current);
    for (int i = 0; i < ws->count; ++i) {
        char name[256];
        workspace_name(ws, i, name, sizeof(name));
        vela_buffer_append(out, i ? "," : "");
        vela_buffer_append_json(out, name);
    }
    vela_buffer_append(out, "],\"windows\":{");
    bool first = true;
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (!view->mapped || !view_id(view)) {
            continue;
        }
        vela_buffer_append(out, first ? "" : ",");
        vela_buffer_append_json(out, view_id(view));
        vela_buffer_appendf(out, ":%d", view->sticky ? -1 : view->workspace);
        first = false;
    }
    vela_buffer_append(out, "},\"stickyApps\":[");
    for (int i = 0; i < ws->sticky_count; ++i) {
        vela_buffer_append(out, i ? "," : "");
        vela_buffer_append_json(out, ws->sticky_apps[i]);
    }
    // The snap groups:
    // [{"output":name,"windows":[{"id":...,"tile":[x0,y0,x1,y1]}]}], in the
    // order their windows appear.
    vela_buffer_append(out, "],\"snapGroups\":[");
    bool first_group = true;
    struct vela_view *leader;
    wl_list_for_each (leader, &server->views, link) {
        uint32_t group = leader->snap_group;
        bool seen = group == 0;
        struct vela_view *earlier;
        wl_list_for_each (earlier, &server->views, link) {
            if (earlier == leader) {
                break;
            }
            seen = seen || earlier->snap_group == group;
        }
        if (seen) {
            continue;
        }
        struct vela_buffer members = { 0 };
        struct vela_output *output = NULL;
        struct vela_view *member;
        wl_list_for_each (member, &server->views, link) {
            if (member->snap_group != group || !member->mapped || !view_id(member)) {
                continue;
            }
            output = output ? output : vela_view_output(member);
            vela_buffer_append(&members, members.length ? ",{\"id\":" : "{\"id\":");
            vela_buffer_append_json(&members, view_id(member));
            struct vela_snap s = member->snap;
            vela_buffer_appendf(&members, ",\"tile\":[%d,%d,%d,%d]}", s.x0, s.y0, s.x1, s.y1);
        }
        if (members.length) {
            vela_buffer_append(out, first_group ? "{\"output\":" : ",{\"output\":");
            vela_buffer_append_json(out, output ? output->wlr->name : "");
            vela_buffer_appendf(out, ",\"windows\":[%s]}", vela_buffer_text(&members));
            first_group = false;
        }
        vela_buffer_finish(&members);
    }
    vela_buffer_append(out, "]}");
}

void vela_workspaces_announce(struct vela_server *server)
{
    struct vela_buffer line = { 0 };
    vela_buffer_append(&line, "workspaces ");
    vela_workspaces_json(server, &line);
    vela_shell_send(server, vela_buffer_text(&line));
    vela_buffer_finish(&line);
}

// ---------------------------------------------------------------- windows --

static int compare_map_serial(const void *a, const void *b)
{
    uint64_t x = (*(struct vela_view *const *)a)->map_serial;
    uint64_t y = (*(struct vela_view *const *)b)->map_serial;
    return x < y ? -1 : x > y;
}

// The taskbar handles: only the windows of the current desktop, in opening
// order.
static void sync_taskbar(struct vela_server *server)
{
    // In opening order: the taskbar shows them in that order.
    int count = wl_list_length(&server->views);
    struct vela_view **ordered = calloc((size_t)(count ? count : 1), sizeof(*ordered));
    int n = 0;
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        ordered[n++] = view;
    }
    qsort(ordered, (size_t)n, sizeof(*ordered), compare_map_serial);
    for (int i = 0; i < n; ++i) {
        if (ordered[i]->mapped) {
            vela_view_show_in_taskbar(ordered[i], vela_view_on_current_workspace(ordered[i]));
        }
    }
    free(ordered);
}

static bool is_sticky_app(const struct vela_workspaces *ws, const char *app_id)
{
    for (int i = 0; i < ws->sticky_count; ++i) {
        if (strcmp(ws->sticky_apps[i], app_id) == 0) {
            return true;
        }
    }
    return false;
}

void vela_workspaces_view_mapped(struct vela_server *server, struct vela_view *view)
{
    struct vela_workspaces *ws = server->workspaces;
    view->map_serial = ws->next_map_serial++;
    view->workspace = ws->current;
    view->sticky = is_sticky_app(ws, vela_view_app_id(view));
}

void vela_workspaces_forget(struct vela_server *server, struct vela_view *view)
{
    struct vela_workspaces *ws = server->workspaces;
    int kept = 0;
    for (int i = 0; i < ws->outgoing_count; ++i) {
        if (ws->outgoing[i] != view) {
            ws->outgoing[kept++] = ws->outgoing[i];
        }
    }
    ws->outgoing_count = kept;
}

static bool is_outgoing(const struct vela_workspaces *ws, const struct vela_view *view)
{
    for (int i = 0; i < ws->outgoing_count; ++i) {
        if (ws->outgoing[i] == view) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- switch --

void vela_workspaces_switch(struct vela_server *server, int index, bool refocus_after)
{
    struct vela_workspaces *ws = server->workspaces;
    if (index < 0 || index >= ws->count || index == ws->current || server->locked) {
        return;
    }
    finish_switch(server); // a switch still in progress ends at once
    int previous = ws->current;
    ws->current = index;
    ws->direction = index > previous ? 1 : -1;

    // The outgoing ones, bottom to top: into layers.windows_out, in the same
    // order.
    struct vela_node *node, *next;
    wl_list_for_each_safe (node, next, &server->layers.windows->children, link) {
        struct vela_owner *owner = node->data;
        if (!owner || owner->kind != VELA_OWNER_VIEW) {
            continue;
        }
        struct vela_view *view = (struct vela_view *)owner;
        if (view->sticky || view->workspace != previous || !view->mapped || view->minimized) {
            continue;
        }
        vela_node_reparent(&view->tree->node, server->layers.windows_out);
        ws->outgoing = vela_grow(ws->outgoing, &ws->outgoing_capacity, ws->outgoing_count + 1, sizeof(*ws->outgoing));
        ws->outgoing[ws->outgoing_count++] = view;
    }
    // All the others: on if they belong to the new desktop. Fullscreen ones
    // are in another layer and change without sliding.
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->mapped && !view->minimized && !is_outgoing(ws, view)) {
            vela_node_set_enabled(&view->tree->node, vela_view_on_current_workspace(view));
        }
    }
    if (server->cursor_mode != VELA_CURSOR_PASSTHROUGH && server->grabbed
        && !vela_view_on_current_workspace(server->grabbed)) {
        vela_snap_end_zone(server, false);
        server->grabbed = NULL;
        server->cursor_mode = VELA_CURSOR_PASSTHROUGH;
    }

    vela_tween_start(&ws->tween, "desktop", vela_motion_ms(VELA_WORKSPACE_SWITCH_MS), NULL);
    vela_node_set_position(&server->layers.windows_out->node, 0, 0);
    vela_node_set_opacity(&server->layers.windows_out->node, 1.0f);
    vela_node_set_opacity(&server->layers.windows->node, 0.0f);
    vela_server_schedule_frames(server);

    sync_taskbar(server);
    if (refocus_after) {
        struct vela_view *focused = vela_views_focused(server);
        if (!focused || !vela_view_on_current_workspace(focused)) {
            vela_focus_refocus(server);
        }
    }
    wlr_log(WLR_INFO, "Desktop %d of %d", index + 1, ws->count);
    vela_workspaces_announce(server);
}

bool vela_workspaces_tick(struct vela_server *server, double now_ms, double wall_ms)
{
    struct vela_workspaces *ws = server->workspaces;
    if (ws->direction == 0) {
        return false;
    }
    double x = vela_tween_progress(&ws->tween, now_ms);
    double t = vela_curve_eval(&vela_decelerate, x);
    int narrowest = 0;
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        int width = 0;
        int height = 0;
        wlr_output_effective_resolution(output->wlr, &width, &height);
        narrowest = narrowest == 0 ? width : vela_min(narrowest, width);
    }
    // Without animation effects the desktops cross-fade where they are.
    double distance = vela_motion_effects() ? narrowest * VELA_WORKSPACE_SLIDE_FRACTION * ws->direction : 0.0;
    struct vela_layers *layers = &server->layers;
    vela_node_set_position(&layers->windows_out->node, -distance * t, 0);
    vela_node_set_opacity(&layers->windows_out->node, (float)(1.0 - t));
    // The new ones turn opaque at once: fading together they would show
    // through each other.
    vela_node_set_position(&layers->windows->node, distance * (1.0 - t), 0);
    vela_node_set_opacity(&layers->windows->node, (float)fmin(1.0, x * 4.0));
    if (vela_motion_logger) {
        // The two desktops' offsets, the one being left and the new one.
        wlr_log(WLR_INFO, "animation: desktop #%u geometry %.3f %.3f 1 1 opacity %.3f", ws->tween.serial,
            -distance * t, distance * (1.0 - t), 1.0 - t);
    }
    if (vela_tween_retired(&ws->tween, wall_ms)) {
        finish_switch(server);
        return false;
    }
    return true;
}

static void finish_switch(struct vela_server *server)
{
    struct vela_workspaces *ws = server->workspaces;
    if (ws->direction == 0) {
        return;
    }
    ws->direction = 0;
    struct vela_layers *layers = &server->layers;
    // The outgoing ones go back to their layer, off, below the others and in
    // their order.
    for (int i = ws->outgoing_count - 1; i >= 0; --i) {
        struct vela_view *view = ws->outgoing[i];
        vela_node_reparent(&view->tree->node, layers->windows);
        struct wl_list *children = &layers->windows->children;
        if (wl_list_length(children) > 1) {
            struct vela_node *first = wl_container_of(children->next, first, link);
            if (first == &view->tree->node) {
                first = wl_container_of(first->link.next, first, link);
            }
            vela_node_place_below(&view->tree->node, first);
        }
        vela_node_set_enabled(&view->tree->node, vela_view_on_current_workspace(view) && !view->minimized);
    }
    ws->outgoing_count = 0;
    vela_node_set_position(&layers->windows_out->node, 0, 0);
    vela_node_set_opacity(&layers->windows_out->node, 1.0f);
    vela_node_set_position(&layers->windows->node, 0, 0);
    vela_node_set_opacity(&layers->windows->node, 1.0f);
    vela_scene_changed(server->scene);
}

// -------------------------------------------------------------- desktop list --

int vela_workspaces_add(struct vela_server *server)
{
    struct vela_workspaces *ws = server->workspaces;
    push_string(&ws->names, &ws->count, &ws->capacity, "");
    save(ws);
    vela_workspaces_announce(server);
    return ws->count - 1;
}

void vela_workspaces_remove(struct vela_server *server, int index)
{
    struct vela_workspaces *ws = server->workspaces;
    if (index < 0 || index >= ws->count || ws->count <= 1) {
        return;
    }
    finish_switch(server);
    // Like Windows: the windows move to the desktop on the left (or on the
    // right, if it was the first).
    int target = index > 0 ? index - 1 : 0; // after removal, the first becomes 0
    bool was_current = index == ws->current;
    if (was_current) {
        vela_workspaces_switch(server, index > 0 ? index - 1 : 1, false);
        finish_switch(server);
    }
    free(ws->names[index]);
    memmove(&ws->names[index], &ws->names[index + 1], (size_t)(ws->count - index - 1) * sizeof(*ws->names));
    --ws->count;
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (view->workspace == index) {
            view->workspace = target;
        } else if (view->workspace > index) {
            --view->workspace;
        }
    }
    if (ws->current > index) {
        --ws->current;
    }
    // The windows that arrived on the current desktop turn on.
    wl_list_for_each (view, &server->views, link) {
        if (view->mapped && !view->minimized) {
            vela_node_set_enabled(&view->tree->node, vela_view_on_current_workspace(view));
        }
    }
    sync_taskbar(server);
    if (was_current) {
        vela_focus_refocus(server);
    }
    save(ws);
    vela_workspaces_announce(server);
}

void vela_workspaces_rename(struct vela_server *server, int index, const char *name)
{
    struct vela_workspaces *ws = server->workspaces;
    if (index < 0 || index >= ws->count) {
        return;
    }
    char *clean = malloc(strlen(name) + 1);
    size_t n = 0;
    for (const char *c = name; *c; ++c) {
        if (*c != '\n' && *c != '\r') {
            clean[n++] = *c;
        }
    }
    clean[n] = '\0';
    // The default name isn't saved, so it follows the numbering.
    char fallback[32];
    snprintf(fallback, sizeof(fallback), "Desktop %d", index + 1);
    if (strcmp(clean, fallback) == 0) {
        clean[0] = '\0';
    }
    free(ws->names[index]);
    ws->names[index] = clean;
    save(ws);
    vela_workspaces_announce(server);
}

// Where desktop `i` ends up when the one at `from` moves to `to`.
static int moved_index(int i, int from, int to)
{
    if (i == from) {
        return to;
    }
    if (from < to && i > from && i <= to) {
        return i - 1;
    }
    if (from > to && i >= to && i < from) {
        return i + 1;
    }
    return i;
}

void vela_workspaces_move(struct vela_server *server, int from, int to)
{
    struct vela_workspaces *ws = server->workspaces;
    if (from < 0 || to < 0 || from >= ws->count || to >= ws->count || from == to) {
        return;
    }
    finish_switch(server);
    // Default names follow the position; chosen ones follow the desktop.
    char *name = ws->names[from];
    if (from < to) {
        memmove(&ws->names[from], &ws->names[from + 1], (size_t)(to - from) * sizeof(*ws->names));
    } else {
        memmove(&ws->names[to + 1], &ws->names[to], (size_t)(from - to) * sizeof(*ws->names));
    }
    ws->names[to] = name;
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        view->workspace = moved_index(view->workspace, from, to);
    }
    ws->current = moved_index(ws->current, from, to);
    save(ws);
    vela_workspaces_announce(server);
}

void vela_workspaces_move_view(struct vela_server *server, struct vela_view *view, int index)
{
    struct vela_workspaces *ws = server->workspaces;
    if (!view || index < 0 || index >= ws->count) {
        return;
    }
    view->sticky = false;
    view->workspace = index;
    vela_view_leave_snap_group(view); // the group stays on the other desktop
    bool was_focused = vela_views_focused(server) == view;
    if (view->mapped && !view->minimized) {
        vela_node_set_enabled(&view->tree->node, vela_view_on_current_workspace(view));
    }
    sync_taskbar(server);
    if (was_focused && !vela_view_on_current_workspace(view)) {
        vela_view_set_activated(view, false);
        wl_list_remove(&view->link);
        wl_list_insert(server->views.prev, &view->link);
        vela_focus_refocus(server);
    }
    vela_workspaces_announce(server);
}

void vela_workspaces_set_sticky(struct vela_server *server, struct vela_view *view, bool on)
{
    if (!view || view->sticky == on) {
        return;
    }
    view->sticky = on;
    // Taken off "all desktops", it stays on the current one.
    view->workspace = server->workspaces->current;
    sync_taskbar(server);
    vela_workspaces_announce(server);
}

void vela_workspaces_set_app_sticky(struct vela_server *server, const char *app_id, bool on)
{
    struct vela_workspaces *ws = server->workspaces;
    if (!app_id || !*app_id) {
        return;
    }
    int kept = 0;
    for (int i = 0; i < ws->sticky_count; ++i) {
        if (strcmp(ws->sticky_apps[i], app_id) == 0) {
            free(ws->sticky_apps[i]);
        } else {
            ws->sticky_apps[kept++] = ws->sticky_apps[i];
        }
    }
    ws->sticky_count = kept;
    if (on) {
        push_string(&ws->sticky_apps, &ws->sticky_count, &ws->sticky_capacity, app_id);
    }
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (strcmp(vela_view_app_id(view), app_id) == 0) {
            view->sticky = on;
            view->workspace = ws->current;
            if (view->mapped && !view->minimized) {
                vela_node_set_enabled(&view->tree->node, true);
            }
        }
    }
    sync_taskbar(server);
    save(ws);
    vela_workspaces_announce(server);
}
