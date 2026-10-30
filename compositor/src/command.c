// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "command.h"

#include "a11y.h"
#include "bindings.h"
#include "brightness.h"
#include "buffer.h"
#include "decoration.h"
#include "input.h"
#include "layer.h"
#include "lock.h"
#include "output.h"
#include "output_manager.h"
#include "render/renderer.h"
#include "scene/frame.h"
#include "scene/scene.h"
#include "server.h"
#include "switcher.h"
#include "util.h"
#include "view.h"
#include "workspace.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#define MAX_LINE 4096 // a connection sending more is closed

// What a connection can follow with "watch <topic>" (docs/brightness.md
// §7.2): a name, and the whole state as one line.
struct topic {
    const char *name;
    void (*write)(struct vela_server *server, struct vela_buffer *line);
};

static const struct topic topics[] = {
    { "brightness", vela_brightness_json },
};
#define TOPICS ((int)(sizeof(topics) / sizeof(topics[0])))

// A connection to the command socket.
struct client {
    struct wl_list link; // vela_commands.clients
    struct vela_server *server;
    int fd;
    struct wl_event_source *source;
    char buffer[MAX_LINE];
    size_t length;
    // Watching: per topic, the newest line not yet started (only the newest
    // matters, every line is the whole state), and the line being written.
    bool watching[TOPICS];
    char *queued[TOPICS];
    char *out;
    size_t out_length, out_done;
    bool brightness_live; // shows a brightness slider (brightness-live on)
};

struct vela_commands {
    int fd;
    struct wl_event_source *source;
    char path[108];
    struct wl_list clients;
};

static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

// Apply a caffeine request on the compositor thread. The confirmed command
// socket variant returns the authoritative resulting state to the shell.
static bool set_caffeine_command(struct vela_server *server, const char *arg)
{
    if (starts_with(arg, "on ")) {
        const char *value = arg + 3;
        char *end = NULL;
        errno = 0;
        const double minutes = strtod(value, &end);
        if (end == value || *end != '\0' || errno == ERANGE || !isfinite(minutes)
            || minutes <= 0 || minutes > VELA_CAFFEINE_MAX_MINUTES) {
            wlr_log(WLR_ERROR, "Invalid caffeine duration (expected >0 to 1440 minutes)");
            return false; // Never mutate existing state for invalid input.
        }
        if (!vela_lock_set_caffeine_for(server->lock, minutes)) {
            vela_a11y_announce(server->a11y); // Includes fail-closed timer errors.
            return false;
        }
    } else if (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0 || strcmp(arg, "toggle") == 0) {
        const bool on = strcmp(arg, "toggle") == 0 ? !server->lock->caffeine : strcmp(arg, "on") == 0;
        vela_lock_set_caffeine(server->lock, on);
    } else {
        wlr_log(WLR_ERROR, "Unknown caffeine command");
        return false;
    }
    vela_a11y_announce(server->a11y);
    return true;
}

// -------------------------------------------------------------- answers --

static void state_json(struct vela_server *server, struct vela_buffer *json)
{
    const char *t = "true";
    const char *f = "false";
    struct vela_view *focused = vela_views_focused(server);
    vela_buffer_appendf(json, "{\"locked\":%s,\"workspace\":%d,\"workspaces\":%d,\"focused\":",
        server->locked ? t : f, server->workspaces->current, server->workspaces->count);
    struct vela_readback_stats readbacks = vela_renderer_readback_stats(server->renderer);
    // The private state socket exposes retention and terminal counts so GPU
    // lifecycle tests can prove they cancelled real in-flight work.
    if (focused && focused->ext_handle && focused->ext_handle->identifier) {
        vela_buffer_append_json(json, focused->ext_handle->identifier);
    } else {
        vela_buffer_append(json, "null");
    }
    vela_buffer_appendf(json, ",\"readback\":{\"pending\":%zu,\"bytes\":%zu,\"completed\":%llu,\"cancelled\":%llu,\"failed\":%llu,\"rejected\":%llu,\"submitted\":%llu}",
        readbacks.pending, readbacks.bytes, (unsigned long long)readbacks.completed,
        (unsigned long long)readbacks.cancelled, (unsigned long long)readbacks.failed,
        (unsigned long long)readbacks.rejected, (unsigned long long)readbacks.submitted);
    vela_buffer_appendf(json, ",\"held\":%llu,\"outputs\":[", (unsigned long long)server->ready.held_total);
    bool first = true;
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        struct wlr_box box = vela_output_box(output);
        vela_buffer_append(json, first ? "{\"name\":" : ",{\"name\":");
        vela_buffer_append_json(json, output->wlr->name);
        vela_buffer_appendf(json,
            ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"scale\":%g,\"transform\":%d,\"enabled\":%s,\"powered\":%s,\"frames\":%llu,"
            "\"missed\":%llu,\"scanout\":%s,\"tearing\":%s,\"deliveredScanout\":%s,\"deliveredTearing\":%s,\"scanoutFeedback\":%s}",
            box.x, box.y, box.width, box.height, (double)output->wlr->scale, (int)output->wlr->transform, output->wlr->enabled ? t : f,
            output->powered ? t : f, (unsigned long long)output->clock.frames_total,
            (unsigned long long)output->clock.missed_total,
            output->frame && output->frame->scanout ? t : f,
            output->frame && output->frame->tearing ? t : f,
            output->frame && output->frame->delivered.scanout ? t : f,
            output->frame && output->frame->delivered.tearing ? t : f,
            output->frame && output->frame->feedback_surface ? t : f);
        first = false;
    }
    vela_buffer_append(json, "],\"windows\":[");
    first = true;
    struct vela_view *view; // order of recent use: the first is on top
    wl_list_for_each (view, &server->views, link) {
        if (!view->mapped || !view->ext_handle || !view->ext_handle->identifier) {
            continue;
        }
        struct wlr_box frame = vela_view_frame_box(view);
        struct vela_output *out = vela_view_output(view);
        vela_buffer_append(json, first ? "{\"id\":" : ",{\"id\":");
        vela_buffer_append_json(json, view->ext_handle->identifier);
        vela_buffer_append(json, ",\"app\":");
        vela_buffer_append_json(json, vela_view_app_id(view));
        vela_buffer_append(json, ",\"title\":");
        vela_buffer_append_json(json, vela_view_title(view));
        vela_buffer_appendf(json, ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"output\":", frame.x, frame.y,
            frame.width, frame.height);
        if (out) {
            vela_buffer_append_json(json, out->wlr->name);
        } else {
            vela_buffer_append(json, "null");
        }
        vela_buffer_appendf(json,
            ",\"workspace\":%d,\"sticky\":%s,\"maximized\":%s,\"minimized\":%s,\"fullscreen\":%s,\"below\":%s,\"snap\":",
            view->workspace, view->sticky ? t : f, view->maximized ? t : f, view->minimized ? t : f,
            view->fullscreen ? t : f, vela_view_stays_below(view) ? t : f);
        if (vela_snap_is_none(view->snap)) {
            vela_buffer_append(json, "null");
        } else {
            vela_buffer_appendf(json, "[%d,%d,%d,%d]", view->snap.x0, view->snap.y0, view->snap.x1, view->snap.y1);
        }
        vela_buffer_appendf(json, ",\"snapGroup\":%u}", view->snap_group);
        first = false;
    }
    // The layer-shell surfaces, bottom to top, and the one with the keyboard.
    vela_buffer_append(json, "],\"layers\":[");
    first = true;
    struct vela_layer_surface *layer;
    struct vela_layer_surface *keyboard_layer = NULL;
    struct wlr_surface *keyboard_surface = server->seat->keyboard_state.focused_surface;
    wl_list_for_each (layer, &server->layer_surfaces, link) {
        const struct wlr_layer_surface_v1 *wlr = layer->wlr;
        struct vela_output *out = vela_layer_surface_output(layer);
        vela_buffer_append(json, first ? "{\"namespace\":" : ",{\"namespace\":");
        vela_buffer_append_json(json, wlr->namespace ? wlr->namespace : "");
        vela_buffer_appendf(json, ",\"layer\":%d,\"keyboard\":%d,\"mapped\":%s,\"output\":", (int)wlr->current.layer,
            (int)wlr->current.keyboard_interactive, wlr->surface->mapped ? t : f);
        if (out) {
            vela_buffer_append_json(json, out->wlr->name);
        } else {
            vela_buffer_append(json, "null");
        }
        double lx = 0, ly = 0;
        vela_node_coords(&layer->tree->node, &lx, &ly);
        vela_buffer_appendf(json, ",\"x\":%g,\"y\":%g,\"w\":%d,\"h\":%d}", lx, ly,
            wlr->surface->current.width, wlr->surface->current.height);
        if (keyboard_surface && wlr->surface == keyboard_surface) {
            keyboard_layer = layer;
        }
        first = false;
    }
    vela_buffer_append(json, "],\"focusedLayer\":");
    if (keyboard_layer) {
        vela_buffer_append_json(json, keyboard_layer->wlr->namespace ? keyboard_layer->wlr->namespace : "");
    } else {
        vela_buffer_append(json, "null");
    }
    vela_buffer_append(json, "}");
}

// The visible windows of the current desktop, topmost first, with their frame
// (bar included): for the Snipping Tool.
static void window_rects_json(struct vela_server *server, struct vela_buffer *json)
{
    // [{"id":...,"title":...,"x":..,"y":..,"w":..,"h":..}], topmost first.
    vela_buffer_append(json, "[");
    bool first = true;
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        if (!view->mapped || view->minimized || !vela_view_on_current_workspace(view) || !view->ext_handle) {
            continue;
        }
        struct wlr_box frame = vela_view_frame_box(view);
        vela_buffer_append(json, first ? "{\"id\":" : ",{\"id\":");
        vela_buffer_append_json(json, view->ext_handle->identifier);
        vela_buffer_append(json, ",\"title\":");
        vela_buffer_append_json(json, vela_view_title(view));
        vela_buffer_appendf(json, ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", frame.x, frame.y, frame.width,
            frame.height);
        first = false;
    }
    vela_buffer_append(json, "]");
}

// Questions with an answer: false if the line isn't one of them.
static bool answer(struct vela_server *server, int fd, const char *line)
{
    struct vela_buffer reply = { 0 };
    if (strcmp(line, "modifiers") == 0) {
        // The shell doesn't have the keyboard when the taskbar is clicked, so
        // it doesn't know whether Shift is held (Shift+right click: the window
        // menu).
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
        vela_buffer_appendf(&reply, "%u", keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0);
    } else if (strcmp(line, "workspaces") == 0) {
        vela_workspaces_json(server, &reply); // for a shell that has just started
    } else if (strcmp(line, "window-rects") == 0) {
        window_rects_json(server, &reply);
    } else if (strcmp(line, "state") == 0) {
        state_json(server, &reply);
    } else if (strcmp(line, "scanout-policy") == 0) {
        // Settings must report the compositor's environment, not its own.
        vela_buffer_appendf(&reply, "{\"envDisabled\":%s,\"enabled\":%s}",
            vela_env_off("VELA_SCANOUT") ? "true" : "false",
            server->scene->allow_scanout ? "true" : "false");
    } else if (strcmp(line, "brightness") == 0) {
        vela_brightness_json(server, &reply);
    } else if (strcmp(line, "caffeine status") == 0) {
        vela_buffer_append(&reply, server->lock->caffeine ? "on" : "off");
    } else if (starts_with(line, "caffeine-set ")) {
        // Unlike the fire-and-forget CLI, the shell waits for this reply.
        // Apply first, then report the resulting compositor state, not the
        // requested one. A failed delivery cannot masquerade as "off".
        const bool ok = set_caffeine_command(server, line + 13);
        char json[512];
        if (!vela_a11y_json(server->a11y, json, sizeof(json))) {
            return true;
        }
        vela_buffer_appendf(&reply, "{\"ok\":%s,\"state\":%s}", ok ? "true" : "false", json);
    } else if (strcmp(line, "accessibility") == 0) {
        char json[512];
        if (!vela_a11y_json(server->a11y, json, sizeof(json))) {
            return true; // doesn't fit: no answer
        }
        vela_buffer_append(&reply, json);
    } else {
        return false;
    }
    vela_buffer_append(&reply, "\n");
    (void)!send(fd, reply.data, reply.length, MSG_NOSIGNAL);
    vela_buffer_finish(&reply);
    return true;
}

// ------------------------------------------------------------ commands --

static void refresh_decorations(struct vela_server *server)
{
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        vela_view_refresh_decoration(view);
    }
}

// "#rrggbb" as an opaque color.
static bool parse_color(const char *text, struct wlr_render_color *color)
{
    unsigned rgb = 0;
    if (strlen(text) != 7 || text[0] != '#' || strspn(text + 1, "0123456789abcdefABCDEF") != 6
        || sscanf(text, "#%6x", &rgb) != 1) {
        return false;
    }
    *color = (struct wlr_render_color) {
        ((rgb >> 16) & 0xff) / 255.0f,
        ((rgb >> 8) & 0xff) / 255.0f,
        (rgb & 0xff) / 255.0f,
        1.0f,
    };
    return true;
}

static bool same_color(const struct wlr_render_color *a, const struct wlr_render_color *b)
{
    return a->r == b->r && a->g == b->g && a->b == b->b && a->a == b->a;
}

// mica #active #inactive: the title bars' colors, computed by the shell
// (shell/src/mica.h, micaFromTint) in the apps' mode.
static void mica(struct vela_server *server, const char *arguments)
{
    char active_text[16] = "";
    char inactive_text[16] = "";
    struct wlr_render_color active;
    struct wlr_render_color inactive;
    if (sscanf(arguments, "%15s %15s", active_text, inactive_text) != 2 || !parse_color(active_text, &active)
        || !parse_color(inactive_text, &inactive)) {
        return;
    }
    if (server->has_mica && same_color(&active, &server->mica_active)
        && same_color(&inactive, &server->mica_inactive)) {
        return;
    }
    server->has_mica = true;
    server->mica_active = active;
    server->mica_inactive = inactive;
    ++server->look_version;
    refresh_decorations(server);
}

// theme <shell> <apps> [<accent>]: "light" or "dark", Vela's mode and the
// apps' mode, and the accent as #rrggbb (an older shell doesn't send it: the
// default stays).
static void theme(struct vela_server *server, const char *arguments)
{
    char shell_mode[16] = "";
    char app_mode[16] = "";
    char accent_text[16] = "";
    if (sscanf(arguments, "%15s %15s %15s", shell_mode, app_mode, accent_text) < 2) {
        return;
    }
    bool shell_light = strcmp(shell_mode, "light") == 0;
    bool apps_light = strcmp(app_mode, "light") == 0;
    struct wlr_render_color accent = server->accent;
    parse_color(accent_text, &accent);
    if (shell_light == server->light_shell && apps_light == server->light_apps
        && same_color(&accent, &server->accent)) {
        return;
    }
    server->accent = accent;
    server->light_shell = shell_light;
    server->light_apps = apps_light;
    // Windows 11's acrylic tint, dark or light (premultiplied sRGB).
    server->scene->acrylic_tint = shell_light
        ? (struct wlr_render_color) { 0.95f * 0.55f, 0.95f * 0.55f, 0.96f * 0.55f, 0.55f }
        : (struct wlr_render_color) { 0.11f * 0.55f, 0.11f * 0.55f, 0.12f * 0.55f, 0.55f };
    ++server->look_version;
    refresh_decorations(server);
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        vela_output_frame_damage_whole(output->frame);
    }
}

// workspace switch|close <n>, workspace new [switch], workspace rename <n>
// <name>, workspace move <from> <to>: from Task View.
static void workspace(struct vela_server *server, const char *line)
{
    char verb[16] = "";
    int a = -1;
    int b = -1;
    int consumed = 0;
    sscanf(line + 10, "%15s %n", verb, &consumed);
    size_t skip = vela_min((int)strlen(line), 10 + consumed);
    const char *rest = line + skip;
    if (strcmp(verb, "switch") == 0 && sscanf(rest, "%d", &a) == 1) {
        vela_workspaces_switch(server, a, true);
    } else if (strcmp(verb, "new") == 0) {
        int index = vela_workspaces_add(server);
        if (strcmp(rest, "switch") == 0) {
            vela_workspaces_switch(server, index, true);
        }
    } else if (strcmp(verb, "close") == 0 && sscanf(rest, "%d", &a) == 1) {
        vela_workspaces_remove(server, a);
    } else if (strcmp(verb, "rename") == 0 && sscanf(rest, "%d %n", &a, &consumed) >= 1) {
        vela_workspaces_rename(server, a, rest + vela_min((int)strlen(rest), consumed));
    } else if (strcmp(verb, "move") == 0 && sscanf(rest, "%d %d", &a, &b) == 2) {
        vela_workspaces_move(server, a, b);
    }
}

// on, off or toggle.
static bool wanted(const char *what, bool now)
{
    return strcmp(what, "toggle") == 0 ? !now : strcmp(what, "on") == 0;
}

// window <ext-foreign-toplevel identifier | active> <action>: from the window
// menu.
static void window(struct vela_server *server, const char *line)
{
    const char *id = line + 7;
    const char *space = strchr(id, ' ');
    if (!space) {
        return;
    }
    size_t id_length = (size_t)(space - id);
    struct vela_view *target = NULL;
    if (id_length == 6 && strncmp(id, "active", 6) == 0) {
        target = vela_views_focused(server);
    } else {
        struct vela_view *view;
        wl_list_for_each (view, &server->views, link) {
            const char *identifier = view->ext_handle ? view->ext_handle->identifier : NULL;
            if (identifier && strlen(identifier) == id_length && strncmp(identifier, id, id_length) == 0) {
                target = view;
            }
        }
    }
    if (target && !server->locked) {
        vela_window_action(server, target, space + 1);
    }
}

// "End task": kills the processes of that app's windows at once, without
// asking (like Windows).
static void end_task(struct vela_server *server, const char *app_id)
{
    int count = wl_list_length(&server->views);
    pid_t *pids = calloc((size_t)count + 1, sizeof(*pids));
    int n = 0;
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        pid_t pid = vela_view_pid(view);
        bool known = false;
        for (int i = 0; i < n; ++i) {
            known = known || pids[i] == pid;
        }
        if (strcmp(app_id, vela_view_app_id(view)) == 0 && pid > 1 && pid != getpid() && !known) {
            pids[n++] = pid;
        }
    }
    for (int i = 0; i < n; ++i) {
        wlr_log(WLR_INFO, "End task: %s (process %d)", app_id, (int)pids[i]);
        kill(pids[i], SIGKILL);
    }
    free(pids);
}

// brightness <output> <value>, value from 0 to 1 (docs/brightness.md §7.2).
static void brightness(struct vela_server *server, const char *arguments)
{
    const char *space = strrchr(arguments, ' ');
    char name[64];
    if (!space || space == arguments || (size_t)(space - arguments) >= sizeof(name)) {
        return;
    }
    memcpy(name, arguments, (size_t)(space - arguments));
    name[space - arguments] = '\0';
    char *end = NULL;
    errno = 0;
    double value = strtod(space + 1, &end);
    if (end == space + 1 || *end != '\0' || errno == ERANGE || !vela_brightness_set(server, name, value)) {
        wlr_log(WLR_DEBUG, "Brightness: \"%s\" not applied", arguments);
    }
}

static void run_command(struct vela_server *server, const char *line)
{
    struct vela_a11y *a11y = server->a11y;
    if (strcmp(line, "logout") == 0) {
        wlr_log(WLR_INFO, "Exit requested by the shell");
        wl_display_terminate(server->display);
    } else if (starts_with(line, "mica ")) {
        mica(server, line + 5);
    } else if (starts_with(line, "theme ")) {
        theme(server, line + 6);
    } else if (strcmp(line, "paste") == 0) {
        vela_input_paste(server->input); // Win+V: the shell has put the item on the clipboard
    } else if (strcmp(line, "modifiers") == 0 || strcmp(line, "workspaces") == 0
        || strcmp(line, "accessibility") == 0 || strcmp(line, "window-rects") == 0
        || strcmp(line, "caffeine status") == 0 || starts_with(line, "caffeine-set ")
        || strcmp(line, "brightness") == 0 || starts_with(line, "watch ") || starts_with(line, "brightness-live ")) {
        // already answered to whoever asked, or about the connection itself
    } else if (starts_with(line, "brightness-probe ")) {
        vela_brightness_probe(server, line + 17); // Settings is showing this output
    } else if (starts_with(line, "brightness ")) {
        brightness(server, line + 11);
    } else if (starts_with(line, "workspace ")) {
        workspace(server, line);
    } else if (strcmp(line, "reload-config") == 0) {
        // Settings changed vela.conf.
        wlr_log(WLR_INFO, "Settings: reloading vela.conf");
        vela_lock_load_settings(server->lock);
        vela_input_reload(server->input); // keyboards, mice and touchpads
        vela_output_load_settings(server);
        vela_brightness_load_settings(server);
        vela_a11y_load(a11y);
    } else if (starts_with(line, "night-light ")) {
        // From quick settings: night-light on|off|toggle (and likewise
        // filters, magnifier, sticky keys).
        vela_a11y_set_night_light(a11y, wanted(line + 12, a11y->night_light), true);
    } else if (starts_with(line, "color-filter ")) {
        vela_a11y_set_color_filter(a11y, wanted(line + 13, a11y->color_filter), true);
    } else if (starts_with(line, "magnifier ")) {
        vela_a11y_set_magnifier(a11y, wanted(line + 10, a11y->magnifier));
    } else if (starts_with(line, "sticky-keys ")) {
        vela_a11y_set_sticky_keys(a11y, wanted(line + 12, a11y->sticky_keys), true);
    } else if (starts_with(line, "caffeine ")) {
        // Existing CLI remains fire-and-forget; only the shell uses the
        // caffeine-set command with a synchronized state reply.
        (void)set_caffeine_command(server, line + 9);
    } else if (starts_with(line, "switcher-pick ")) {
        // A click on an Alt+Tab preview: switch to that window.
        vela_switcher_pick(server, atoi(line + 14));
    } else if (starts_with(line, "test-output ")) {
        vela_test_output_command(server, line + 12);
    } else if (strcmp(line, "test-power off") == 0 || strcmp(line, "test-power on") == 0) {
        // For tests: turns outputs off and on like inactivity does, headless
        // ones only (like test-output).
        struct vela_output *output;
        wl_list_for_each (output, &server->outputs, link) {
            if (wlr_output_is_headless(output->wlr)) {
                vela_output_set_powered(output, strcmp(line, "test-power on") == 0);
            }
        }
    } else if (strcmp(line, "lock") == 0) {
        vela_lock_screen(server->lock); // for example before suspending the computer
    } else if (starts_with(line, "window ")) {
        window(server, line);
    } else if (starts_with(line, "end-task ")) {
        end_task(server, line + 9);
    } else if (line[0]) {
        wlr_log(WLR_DEBUG, "Unknown command: %s", line);
    }
}

// -------------------------------------------------------------- watching --

static int topic_index(const char *name)
{
    for (int i = 0; i < TOPICS; ++i) {
        if (strcmp(topics[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void stop_watching(struct client *client)
{
    for (int i = 0; i < TOPICS; ++i) {
        client->watching[i] = false;
        free(client->queued[i]);
        client->queued[i] = NULL;
    }
    free(client->out);
    client->out = NULL;
}

// Writes what it can without blocking; the rest when the socket has room. A
// watcher that stops reading misses states in between, never the last one.
static void flush(struct client *client)
{
    for (;;) {
        if (!client->out) {
            for (int i = 0; i < TOPICS && !client->out; ++i) {
                if (client->queued[i]) {
                    client->out = client->queued[i];
                    client->out_length = strlen(client->out);
                    client->out_done = 0;
                    client->queued[i] = NULL;
                }
            }
            if (!client->out) {
                break;
            }
        }
        ssize_t n = send(client->fd, client->out + client->out_done, client->out_length - client->out_done,
            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (n <= 0) {
            stop_watching(client); // gone: the hangup closes it
            break;
        }
        client->out_done += (size_t)n;
        if (client->out_done == client->out_length) {
            free(client->out);
            client->out = NULL;
        }
    }
    wl_event_source_fd_update(client->source, WL_EVENT_READABLE | (client->out ? WL_EVENT_WRITABLE : 0));
}

static void queue(struct client *client, int topic, const char *line)
{
    free(client->queued[topic]);
    client->queued[topic] = strdup(line);
    flush(client);
}

static char *topic_line(struct vela_server *server, int topic)
{
    struct vela_buffer line = { 0 };
    topics[topic].write(server, &line);
    vela_buffer_append(&line, "\n");
    char *text = strdup(vela_buffer_text(&line));
    vela_buffer_finish(&line);
    return text;
}

void vela_commands_notify(struct vela_server *server, const char *name)
{
    int topic = topic_index(name);
    if (!server->commands || topic < 0) {
        return;
    }
    char *line = NULL;
    struct client *client;
    wl_list_for_each (client, &server->commands->clients, link) {
        if (client->watching[topic]) {
            if (!line) {
                line = topic_line(server, topic);
            }
            queue(client, topic, line);
        }
    }
    free(line);
}

static void watch(struct client *client, const char *name)
{
    int topic = topic_index(name);
    if (topic < 0) {
        wlr_log(WLR_DEBUG, "Nothing to watch called \"%s\"", name);
        return;
    }
    client->watching[topic] = true;
    char *line = topic_line(client->server, topic);
    queue(client, topic, line); // the state now, then every change
    free(line);
}

// Live sync is on while any connection shows a slider: a client that closes
// counts as off (§7.2).
static void update_brightness_live(struct vela_server *server)
{
    bool live = false;
    struct client *client;
    wl_list_for_each (client, &server->commands->clients, link) {
        live = live || client->brightness_live;
    }
    vela_brightness_live(server, live);
}

// Lines about the connection itself: false if the line isn't one of them.
static bool connection_line(struct client *client, const char *line)
{
    if (starts_with(line, "watch ")) {
        watch(client, line + 6);
    } else if (strcmp(line, "brightness-live on") == 0 || strcmp(line, "brightness-live off") == 0) {
        client->brightness_live = strcmp(line, "brightness-live on") == 0;
        update_brightness_live(client->server);
    } else {
        return false;
    }
    return true;
}

// --------------------------------------------------------------- socket --

static void client_destroy(struct client *client)
{
    bool live = client->brightness_live;
    wl_list_remove(&client->link);
    wl_event_source_remove(client->source);
    close(client->fd);
    stop_watching(client);
    if (live && client->server->commands) {
        update_brightness_live(client->server);
    }
    free(client);
}

static int handle_client(int fd, uint32_t mask, void *data)
{
    struct client *client = data;
    struct vela_server *server = client->server;
    if (mask & WL_EVENT_WRITABLE) {
        flush(client);
    }
    char chunk[256];
    ssize_t n = -1;
    // Bound each event-loop turn to MAX_LINE bytes, not the lifetime of the
    // socket. A stream may contain thousands of short, valid commands. Do
    // not read past the remaining capacity: that would silently discard the
    // tail of a read, then close an otherwise valid connection on a burst.
    // Once this buffer fills, drain its complete lines below and let the
    // still-readable fd wake us again for the next batch.
    while (client->length < MAX_LINE) {
        size_t room = MAX_LINE - client->length;
        size_t amount = room < sizeof(chunk) ? room : sizeof(chunk);
        n = read(fd, chunk, amount);
        if (n <= 0) {
            break;
        }
        memcpy(client->buffer + client->length, chunk, (size_t)n);
        client->length += (size_t)n;
    }
    // The complete lines; the rest waits for the next piece.
    char lines[MAX_LINE + 1];
    size_t lines_length = 0;
    char *newline;
    while ((newline = memchr(client->buffer, '\n', client->length))) {
        size_t line_length = (size_t)(newline - client->buffer);
        memcpy(lines + lines_length, client->buffer, line_length);
        lines[lines_length + line_length] = '\0';
        lines_length += line_length + 1;
        client->length -= line_length + 1;
        memmove(client->buffer, newline + 1, client->length);
    }
    for (size_t at = 0; at < lines_length; at += strlen(lines + at) + 1) {
        if (!connection_line(client, lines + at)) {
            answer(server, fd, lines + at);
        }
    }
    // A full buffer of COMPLETE lines is not an overlong request: removing
    // those lines freed capacity. Only an incomplete line occupying all of
    // MAX_LINE is malformed and must close this client.
    bool closed = n == 0 || (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) || client->length >= MAX_LINE;
    if (closed) {
        client_destroy(client);
    }
    // After the client is settled: a command can close everything.
    for (size_t at = 0; at < lines_length; at += strlen(lines + at) + 1) {
        run_command(server, lines + at);
    }
    return 0;
}

static int handle_connection(int fd, uint32_t mask, void *data)
{
    struct vela_server *server = data;
    int client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0) {
        return 0;
    }
    struct client *client = calloc(1, sizeof(*client));
    client->server = server;
    client->fd = client_fd;
    client->source = wl_event_loop_add_fd(server->loop, client_fd, WL_EVENT_READABLE, handle_client, client);
    wl_list_insert(server->commands->clients.prev, &client->link);
    return 0;
}

void vela_commands_listen(struct vela_server *server)
{
    struct vela_commands *commands = calloc(1, sizeof(*commands));
    commands->fd = -1;
    wl_list_init(&commands->clients);
    server->commands = commands;
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir
        || !vela_format(commands->path, sizeof(commands->path), "%s/vela-%s.sock", runtime_dir,
            server->socket_name)) {
        commands->path[0] = '\0';
        return;
    }
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", commands->path);
    unlink(commands->path); // left over from a previous run
    commands->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (commands->fd < 0 || bind(commands->fd, (struct sockaddr *)&address, sizeof(address)) != 0
        || listen(commands->fd, 4) != 0) {
        wlr_log_errno(WLR_ERROR, "Can't listen for commands on %s", commands->path);
        if (commands->fd >= 0) {
            close(commands->fd);
            commands->fd = -1;
            unlink(commands->path);
        }
        return;
    }
    chmod(commands->path, 0600);
    commands->source = wl_event_loop_add_fd(server->loop, commands->fd, WL_EVENT_READABLE, handle_connection, server);
}

void vela_commands_stop(struct vela_server *server)
{
    struct vela_commands *commands = server->commands;
    if (!commands) {
        return;
    }
    struct client *client, *next;
    wl_list_for_each_safe (client, next, &commands->clients, link) {
        client_destroy(client);
    }
    if (commands->source) {
        wl_event_source_remove(commands->source);
    }
    if (commands->fd >= 0) {
        close(commands->fd);
        unlink(commands->path);
    }
    free(commands);
    server->commands = NULL;
}
