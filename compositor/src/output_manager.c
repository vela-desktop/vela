// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output_manager.h"
#include "alloc/alloc.h"
#include "interact.h"
#include "listen.h"
#include "output.h"
#include "output_arrangement.h"
#include "output_config.h"
#include "output_preview.h"
#include "scene/scene.h"
#include "server.h"
#include "view.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/util/log.h>

struct wlr_output_configuration_v1 *vela_output_manager_snapshot(struct vela_server *server)
{
    struct wlr_output_configuration_v1 *config = VELA_MUST(wlr_output_configuration_v1_create());
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        struct wlr_output_configuration_head_v1 *head
            = VELA_MUST(wlr_output_configuration_head_v1_create(config, output->wlr));
        struct wlr_box box = { 0 };
        wlr_output_layout_get_box(server->output_layout, output->wlr, &box);
        head->state.enabled = output->wlr->enabled && !wlr_box_empty(&box);
        bool remembered = !head->state.enabled && output->has_last_position;
        head->state.x = remembered ? output->last_x : box.x;
        head->state.y = remembered ? output->last_y : box.y;
    }
    return config;
}

void vela_output_manager_update(struct vela_server *server)
{
    wlr_output_manager_v1_set_configuration(server->output_manager, vela_output_manager_snapshot(server));
}

// Unlike Wayland's arbitrary global coordinates, XWayland's X11 root uses
// 0,0 as its origin. Keep the *relative* monitor arrangement (including an
// output above the primary), but translate the entire enabled layout so its
// top-left enabled pixel is (0,0). Do not independently clamp heads: that
// would overlap monitors and break window/output correspondence.
static void configuration_origin(const struct wlr_output_configuration_v1 *config, int *x, int *y)
{
    int min_x = INT_MAX, min_y = INT_MAX;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each (head, &config->heads, link) {
        if (!head->state.enabled) {
            continue;
        }
        if (head->state.x < min_x) {
            min_x = head->state.x;
        }
        if (head->state.y < min_y) {
            min_y = head->state.y;
        }
    }
    *x = min_x == INT_MAX ? 0 : min_x;
    *y = min_y == INT_MAX ? 0 : min_y;
}

static bool test_configuration(const struct wlr_output_configuration_v1 *config, bool restoring)
{
    int origin_x, origin_y;
    configuration_origin(config, &origin_x, &origin_y);
    int count = wl_list_length(&config->heads);
    struct vela_output_rect *rects = VELA_NEW_N(struct vela_output_rect, (size_t)count + 1);
    int n = 0;
    bool valid = true;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each (head, &config->heads, link) {
        const struct wlr_output_head_v1_state *wanted = &head->state;
        if (!wanted->output->data) {
            valid = false;
            break;
        }
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_head_v1_state_apply(wanted, &state);
        valid = wlr_output_test_state(wanted->output, &state);
        wlr_output_state_finish(&state);
        if (!valid) {
            break;
        }
        if (!wanted->enabled) {
            continue;
        }
        int width = wanted->mode ? wanted->mode->width : wanted->custom_mode.width;
        int height = wanted->mode ? wanted->mode->height : wanted->custom_mode.height;
        bool rotated = wanted->transform & 1;
        float logical_width = (float)(rotated ? height : width) / wanted->scale;
        float logical_height = (float)(rotated ? width : height) / wanted->scale;
        if (!isfinite(logical_width) || logical_width < 1 || (double)logical_width > INT_MAX
            || !isfinite(logical_height) || logical_height < 1 || (double)logical_height > INT_MAX) {
            valid = false;
            break;
        }
        struct vela_output_rect rect = {
            (int64_t)wanted->x - origin_x,
            (int64_t)wanted->y - origin_y,
            (int)logical_width,
            (int)logical_height,
        };
        if (rect.x + rect.width > INT_MAX || rect.y + rect.height > INT_MAX) {
            valid = false;
            break;
        }
        for (int i = 0; !restoring && i < n; ++i) {
            if (vela_output_rects_overlap(rect, rects[i])) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            break;
        }
        rects[n++] = rect;
    }
    free(rects);
    return valid;
}

// Remember each mapped window's original output coordinates. A normalization
// translates every monitor together, so its windows (including restored frame
// positions) must follow that output rather than remaining in the old area.
struct vela_tracked_view {
    struct vela_view *view;
    struct wlr_output *output;
    int x, y;
};

static struct vela_tracked_view *track_views(struct vela_server *server, int *count)
{
    int tracked_count = wl_list_length(&server->views);
    struct vela_tracked_view *tracked = VELA_NEW_N(struct vela_tracked_view, (size_t)tracked_count + 1);
    struct vela_view *view;
    int n = 0;
    wl_list_for_each (view, &server->views, link) {
        struct vela_output *old = vela_view_output(view);
        if (old && old->wlr->enabled && wlr_output_layout_get(server->output_layout, old->wlr)) {
            struct wlr_box box = vela_output_box(old);
            tracked[n++] = (struct vela_tracked_view) { view, old->wlr, box.x, box.y };
        }
    }
    *count = n;
    return tracked;
}

static void finish_layout_change(struct vela_server *server, struct vela_tracked_view *tracked, int n)
{
    // Windows must travel with their output when we shift the coordinate
    // origin. Restore boxes are *absolute* frame coordinates as well.
    // This also keeps preview rollback stable when the old layout returns.
    for (int i = 0; i < n; ++i) {
        struct vela_tracked_view *entry = &tracked[i];
        struct wlr_output_layout_output *layout
            = wlr_output_layout_get(server->output_layout, entry->output);
        if (!layout || !entry->output->enabled) {
            continue; // A disabled head is rescued by check_outputs below.
        }
        struct wlr_box new_box = { 0 };
        wlr_output_layout_get_box(server->output_layout, entry->output, &new_box);
        double dx = (double)new_box.x - entry->x;
        double dy = (double)new_box.y - entry->y;
        if (dx || dy) {
            struct vela_view *view = entry->view;
            vela_node_set_position(&view->tree->node, view->tree->node.x + dx, view->tree->node.y + dy);
            if (view->restore.width > 0 && view->restore.height > 0) {
                view->restore.x += dx;
                view->restore.y += dy;
            }
            view->target_x += dx;
            view->target_y += dy;
            vela_view_sync_x11(view);
        }
    }
    free(tracked);
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        vela_output_arrange_layers(output);
    }
    vela_views_check_outputs(server);
    struct vela_view *view;
    wl_list_for_each (view, &server->views, link) {
        vela_view_keep_in_place(view);
        vela_view_sync_x11(view);
    }
    vela_scene_changed(server->scene);
    // Scale/rotation alone may not cause a layout-change notification.
    vela_output_manager_update(server);
}

static void arrange_after_hotplug(void *data)
{
    struct vela_server *server = data;
    server->output_rearrange = NULL;
    vela_output_preview_hotplug(server);

    int count = wl_list_length(&server->outputs);
    struct vela_output **outputs = VELA_NEW_N(struct vela_output *, (size_t)count + 1);
    struct vela_output_rect *rects = VELA_NEW_N(struct vela_output_rect, (size_t)count + 1);
    bool *placed = VELA_NEW_N(bool, (size_t)count + 1);
    int n = 0;
    int anchor = 0;
    struct vela_output *output;
    wl_list_for_each(output, &server->outputs, link)
    {
        struct wlr_box box = vela_output_box(output);
        if (wlr_box_empty(&box)) {
            continue;
        }
        if (!output->has_preferred_position) {
            output->has_preferred_position = true;
            output->preferred_x = box.x;
            output->preferred_y = box.y;
        }
        rects[n] = (struct vela_output_rect) { output->preferred_x, output->preferred_y, box.width, box.height };
        outputs[n] = output;
        // Keep the upper-left anchor. If saved rectangles share an origin,
        // prefer the larger screen so an overlapping laptop panel can move.
        struct vela_output_rect a = rects[anchor];
        struct vela_output_rect b = rects[n];
        if (b.x < a.x || (b.x == a.x && (b.y < a.y || (b.y == a.y && b.width * b.height > a.width * a.height)))) {
            anchor = n;
        }
        ++n;
    }
    if (n == 0) {
        goto done;
    }
    struct vela_output_rect first = rects[0];
    rects[0] = rects[anchor];
    rects[anchor] = first;
    output = outputs[0];
    outputs[0] = outputs[anchor];
    outputs[anchor] = output;
    vela_output_arrange_rects(rects, placed, (size_t)n);

    int64_t min_x = rects[0].x, min_y = rects[0].y;
    for (int i = 1; i < n; ++i) {
        if (rects[i].x < min_x) min_x = rects[i].x;
        if (rects[i].y < min_y) min_y = rects[i].y;
    }
    bool moved = false;
    for (int i = 0; i < n; ++i) {
        rects[i].x -= min_x;
        rects[i].y -= min_y;
        if (rects[i].x + rects[i].width > INT_MAX || rects[i].y + rects[i].height > INT_MAX) {
            wlr_log(WLR_ERROR, "Could not represent the rearranged display layout");
            goto done;
        }
        struct wlr_box box = vela_output_box(outputs[i]);
        moved |= box.x != rects[i].x || box.y != rects[i].y;
    }
    int tracked_count = 0;
    struct vela_tracked_view *tracked = NULL;
    struct vela_output *pointed = NULL;
    double cursor_x = 0.0, cursor_y = 0.0;
    if (moved) {
        // Rescue windows on a removed output before another output takes
        // over its coordinates. Track the surviving windows for translation.
        vela_views_check_outputs(server);
        tracked = track_views(server, &tracked_count);
        if (server->cursor) {
            struct wlr_output *wlr =
                wlr_output_layout_output_at(server->output_layout, server->cursor->x, server->cursor->y);
            pointed = wlr ? wlr->data : NULL;
            if (pointed) {
                struct wlr_box box = vela_output_box(pointed);
                cursor_x = server->cursor->x - box.x;
                cursor_y = server->cursor->y - box.y;
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        struct wlr_output_layout_output *layout = wlr_output_layout_get(server->output_layout, outputs[i]->wlr);
        if ((layout->auto_configured && !outputs[i]->nested) || layout->x != rects[i].x || layout->y != rects[i].y) {
            wlr_output_layout_add(server->output_layout, outputs[i]->wlr, (int)rects[i].x, (int)rects[i].y);
        }
    }
    if (moved) {
        if (pointed) {
            struct wlr_box box = vela_output_box(pointed);
            wlr_cursor_warp_closest(server->cursor, NULL, box.x + cursor_x, box.y + cursor_y);
        }
        finish_layout_change(server, tracked, tracked_count);
        if (server->cursor) {
            vela_interact_rebase(server);
        }
    }

done:
    free(placed);
    free(rects);
    free(outputs);
}

void vela_output_manager_hotplug(struct vela_server *server)
{
    // wlroots removes its layout entry later in the destroy sequence. Wait
    // for that and coalesce hotplug notifications before moving any outputs.
    if (server->output_previews && !server->output_rearrange) {
        server->output_rearrange = VELA_MUST(wl_event_loop_add_idle(server->loop, arrange_after_hotplug, server));
    }
}

static bool commit_configuration(
    struct vela_server *server, const struct wlr_output_configuration_v1 *config, bool restoring)
{
    // Preflight every output before changing any. Commits can still fail, so
    // the caller retains a snapshot for restoration in that case.
    if (!test_configuration(config, restoring)) {
        return false;
    }
    int origin_x, origin_y;
    configuration_origin(config, &origin_x, &origin_y);
    int n;
    struct vela_tracked_view *tracked = track_views(server, &n);
    bool ok = true;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
        const struct wlr_output_head_v1_state *wanted = &head->state;
        struct vela_output *output = wanted->output->data;
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_head_v1_state_apply(wanted, &state);
        if (wanted->enabled) {
            // Render the first new frame at its intended position.
            wlr_output_layout_add(server->output_layout, wanted->output, wanted->x - origin_x, wanted->y - origin_y);
            ok = vela_output_commit_mode(output, &state);
        } else {
            struct wlr_box previous = { 0 };
            wlr_output_layout_get_box(server->output_layout, wanted->output, &previous);
            ok = wlr_output_commit_state(wanted->output, &state);
            if (ok) {
                // disable_head does not carry position. Remember the live
                // layout, or explicitly restore a disabled head's snapshot.
                if (restoring || !wlr_box_empty(&previous)) {
                    output->has_last_position = true;
                    output->last_x = restoring ? wanted->x : previous.x;
                    output->last_y = restoring ? wanted->y : previous.y;
                }
                wlr_output_layout_remove(server->output_layout, wanted->output);
            }
        }
        wlr_output_state_finish(&state);
        if (!ok) {
            break;
        }
    }
    finish_layout_change(server, tracked, n);
    return ok;
}

bool vela_output_manager_commit(struct vela_server *server, const struct wlr_output_configuration_v1 *config)
{
    return commit_configuration(server, config, false);
}

bool vela_output_manager_save(struct vela_server *server, const struct wlr_output_configuration_v1 *before)
{
    struct wlr_output_configuration_v1 *config = vela_output_manager_snapshot(server);
    // Only adopt the live coordinates as the preferred arrangement if the
    // user changed the layout. A refresh-rate change on a temporarily moved
    // display must not make its hotplug fallback permanent.
    bool moved = false;
    struct wlr_output_configuration_head_v1 *head, *old;
    wl_list_for_each(head, &config->heads, link)
    {
        wl_list_for_each(old, &before->heads, link)
        {
            if (head->state.output == old->state.output && head->state.enabled
                && (head->state.x != old->state.x || head->state.y != old->state.y)) {
                moved = true;
                break;
            }
        }
    }
    wl_list_for_each(head, &config->heads, link)
    {
        struct vela_output *output = head->state.output->data;
        if (head->state.enabled && (moved || !output->has_preferred_position)) {
            output->has_preferred_position = true;
            output->preferred_x = head->state.x;
            output->preferred_y = head->state.y;
        }
    }
    if (!server->session) {
        wlr_output_configuration_v1_destroy(config);
        return true; // Non-DRM sessions deliberately do not persist.
    }
    int count = wl_list_length(&config->heads);
    char (*keys)[256] = vela_alloc((size_t)count + 1, sizeof(*keys));
    struct vela_output_config_entry *entries = VELA_NEW_N(struct vela_output_config_entry, (size_t)count + 1);
    int n = 0;
    wl_list_for_each (head, &config->heads, link) {
        const struct wlr_output_head_v1_state *state = &head->state;
        const struct wlr_output *wlr = state->output;
        vela_output_key(wlr, keys[n], sizeof(keys[n]));
        const struct vela_output *output = wlr->data;
        int x = state->enabled && output->has_preferred_position ? output->preferred_x : state->x;
        int y = state->enabled && output->has_preferred_position ? output->preferred_y : state->y;
        entries[n] = (struct vela_output_config_entry) { keys[n], state->enabled, wlr->width, wlr->height, wlr->refresh,
            wlr->scale, wlr->transform, x, y };
        ++n;
    }
    bool saved = vela_output_config_save(entries, n);
    free(entries);
    free(keys);
    wlr_output_configuration_v1_destroy(config);
    return saved;
}

bool vela_output_manager_restore(struct vela_server *server, const struct wlr_output_configuration_v1 *before)
{
    // Include new outputs at their current positions: otherwise the layout's
    // automatic placement can move them as an old output regains its size.
    struct wlr_output_configuration_v1 *restore = vela_output_manager_snapshot(server);
    struct wlr_output_configuration_head_v1 *head, *saved;
    wl_list_for_each (head, &restore->heads, link) {
        wl_list_for_each (saved, &before->heads, link) {
            if (head->state.output == saved->state.output) {
                head->state = saved->state;
                break;
            }
        }
    }
    bool ok = commit_configuration(server, restore, true);
    wlr_output_configuration_v1_destroy(restore);
    return ok;
}

static void apply(struct vela_server *server, struct wlr_output_configuration_v1 *config, bool test_only)
{
    bool allowed = test_only || vela_output_preview_can_apply(server, config);
    struct wlr_output_configuration_v1 *before = !test_only && allowed ? vela_output_manager_snapshot(server) : NULL;
    bool ok = allowed && (test_only ? test_configuration(config, false) : vela_output_manager_commit(server, config));
    bool restored = true;
    if (!ok && before && !vela_output_manager_restore(server, before)) {
        restored = false;
        wlr_log(WLR_ERROR, "Failed to restore outputs after a rejected configuration");
    }
    bool protected = !test_only && vela_output_preview_applied(server, config, before, ok, restored);
    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
        if (!test_only && !protected) {
            if (!vela_output_manager_save(server, before)) {
                wlr_log(WLR_ERROR, "Could not persist the display configuration");
            }
        }
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    if (before && !protected) {
        wlr_output_configuration_v1_destroy(before);
    }
    wlr_output_configuration_v1_destroy(config);
}

static void handle_apply(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, output_manager_apply);
    apply(server, data, false);
}

static void handle_test(struct wl_listener *listener, void *data)
{
    struct vela_server *server = wl_container_of(listener, server, output_manager_test);
    apply(server, data, true);
}

void vela_output_manager_init(struct vela_server *server)
{
    server->output_manager = VELA_MUST(wlr_output_manager_v1_create(server->display));
    vela_listen(&server->output_manager->events.apply, &server->output_manager_apply, handle_apply);
    vela_listen(&server->output_manager->events.test, &server->output_manager_test, handle_test);
    vela_output_preview_init(server);
}

void vela_output_manager_finish(struct vela_server *server)
{
    if (server->output_rearrange) {
        wl_event_source_remove(server->output_rearrange);
        server->output_rearrange = NULL;
    }
    vela_output_preview_finish(server);
    wl_list_remove(&server->output_manager_apply.link);
    wl_list_remove(&server->output_manager_test.link);
}

// ------------------------------------------------------------ test-output --

static void find_headless(struct wlr_backend *candidate, void *data)
{
    if (wlr_backend_is_headless(candidate)) {
        *(struct wlr_backend **)data = candidate;
    }
}

void vela_test_output_command(struct vela_server *server, const char *arguments)
{
    // Only without real outputs: hot plugging and unplugging in tests.
    struct wlr_backend *headless = NULL;
    wlr_multi_for_each_backend(server->backend, find_headless, &headless);
    if (!headless) {
        wlr_log(WLR_ERROR, "test-output: only with the headless backend");
        return;
    }
    int width = 0;
    int height = 0;
    double hz = 0.0; // add 1920x1080@180: with that refresh rate
    char name[64] = "";
    if (sscanf(arguments, "add %dx%d@%lf", &width, &height, &hz) >= 2 && width > 0 && height > 0) {
        struct wlr_output *wlr = wlr_headless_add_output(headless, (unsigned)width, (unsigned)height);
        // VELA_OUTPUT_SIZE applies to the initial outputs: this one has its
        // own.
        struct vela_output *output = wlr ? wlr->data : NULL;
        if (output) {
            struct wlr_output_state state;
            wlr_output_state_init(&state);
            wlr_output_state_set_custom_mode(&state, width, height, (int32_t)(hz * 1000.0));
            vela_output_commit_mode(output, &state);
            wlr_output_state_finish(&state);
        }
        wlr_log(WLR_INFO, "test-output: connected %s (%dx%d@%g)", wlr ? wlr->name : "(none)", width, height, hz);
    } else if (sscanf(arguments, "remove %63s", name) == 1) {
        struct vela_output *output = vela_output_named(server, name);
        if (output) {
            wlr_log(WLR_INFO, "test-output: disconnecting %s", name);
            wlr_output_destroy(output->wlr);
        }
    }
}
