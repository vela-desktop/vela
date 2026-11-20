// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output_manager.h"
#include "alloc/alloc.h"
#include "output_preview.h"

#include "listen.h"
#include "output.h"
#include "output_config.h"
#include "scene/scene.h"
#include "server.h"
#include "view.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/util/log.h>

static struct wlr_output_configuration_v1 *snapshot(struct vela_server *server, bool configured)
{
    struct wlr_output_configuration_v1 *config = VELA_MUST(wlr_output_configuration_v1_create());
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        struct wlr_output_configuration_head_v1 *head
            = VELA_MUST(wlr_output_configuration_head_v1_create(config, output->wlr));
        if (configured && output->lid_saved.enabled) {
            head->state = output->lid_saved;
            continue;
        }
        struct wlr_box box = { 0 };
        wlr_output_layout_get_box(server->output_layout, output->wlr, &box);
        head->state.enabled = (output->wlr->enabled || (configured && !output->powered)) && !wlr_box_empty(&box);
        bool remembered = !head->state.enabled && output->has_last_position;
        head->state.x = remembered ? output->last_x : box.x;
        head->state.y = remembered ? output->last_y : box.y;
    }
    return config;
}

void vela_output_manager_update(struct vela_server *server)
{
    wlr_output_manager_v1_set_configuration(server->output_manager, snapshot(server, false));
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

static bool lid_hides_panels(const struct vela_server *server, const struct wlr_output_configuration_v1 *config)
{
    if (!server->lid_closed) {
        return false;
    }
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each (head, &config->heads, link) {
        const struct vela_output *output = head->state.output->data;
        if (head->state.enabled && output && output->drm_connector && !output->internal_panel) {
            return true;
        }
    }
    return false;
}

static bool test_head_state(const struct wlr_output_head_v1_state *head, bool hide)
{
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    if (hide) {
        wlr_output_state_set_enabled(&state, false);
    } else {
        wlr_output_head_v1_state_apply(head, &state);
    }
    bool ok = wlr_output_test_state(head->output, &state);
    wlr_output_state_finish(&state);
    return ok;
}

static bool test_configuration(
    const struct wlr_output_configuration_v1 *config, bool hide_panels, bool lid_only, int origin_x, int origin_y)
{
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
        const struct vela_output *output = head->state.output->data;
        if (!output) {
            return false;
        }
        bool hide = hide_panels && output->internal_panel && head->state.enabled;
        if (lid_only && hide == output->lid_saved.enabled) {
            continue;
        }
        // The normalized coordinates must remain representable in the
        // output layout's signed 32-bit positions.
        if (head->state.enabled && ((int64_t)head->state.x - origin_x > INT_MAX
            || (int64_t)head->state.y - origin_y > INT_MAX)) {
            return false;
        }
        if (!test_head_state(&head->state, hide)) {
            return false;
        }
    }
    return true;
}

// Remember each mapped window's original output coordinates. A normalization
// translates every monitor together, so its windows (including restored frame
// positions) must follow that output rather than remaining in the old area.
struct vela_tracked_view {
    struct vela_view *view;
    struct wlr_output *output;
    int x, y;
};

static bool commit_configuration(
    struct vela_server *server, const struct wlr_output_configuration_v1 *config, bool restoring, bool lid_only)
{
    // Preflight every output before changing any. Commits can still fail, so
    // the caller retains a snapshot for restoration in that case.
    int origin_x = 0, origin_y = 0;
    if (!lid_only) {
        configuration_origin(config, &origin_x, &origin_y);
    }
    bool hide_panels = lid_hides_panels(server, config);
    if (!test_configuration(config, hide_panels, lid_only, origin_x, origin_y)) {
        return false;
    }
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
    bool ok = true, changed = false;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each (head, &config->heads, link) {
        const struct wlr_output_head_v1_state *wanted = &head->state;
        struct vela_output *output = wanted->output->data;
        bool hide = hide_panels && output->internal_panel && wanted->enabled;
        if (lid_only && hide == output->lid_saved.enabled) {
            continue;
        }
        changed = true;
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        if (wanted->enabled && !hide) {
            wlr_output_head_v1_state_apply(wanted, &state);
            // Render the first new frame at its intended position.
            wlr_output_layout_add(server->output_layout, wanted->output,
                wanted->x - origin_x, wanted->y - origin_y);
            ok = vela_output_commit_mode(output, &state);
            if (ok && output->lid_saved.enabled) {
                output->powered = true;
            }
        } else {
            struct wlr_box previous = { 0 };
            wlr_output_layout_get_box(server->output_layout, wanted->output, &previous);
            wlr_output_state_set_enabled(&state, false);
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
            // An enable attempt adds the output before its first frame.
            // Leave a previously hidden panel outside the layout on failure.
            if (output->lid_saved.enabled) {
                wlr_output_layout_remove(server->output_layout, wanted->output);
            }
            break;
        }
        if (hide) {
            output->lid_saved = *wanted;
            output->lid_saved.x -= origin_x;
            output->lid_saved.y -= origin_y;
        } else {
            output->lid_saved.enabled = false;
        }
    }
    if (!changed) {
        free(tracked);
        return ok;
    }
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
        int dx = new_box.x - entry->x;
        int dy = new_box.y - entry->y;
        if (dx || dy) {
            view = entry->view;
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
    wl_list_for_each (view, &server->views, link) {
        vela_view_keep_in_place(view);
        vela_view_sync_x11(view);
    }
    vela_scene_changed(server->scene);
    // Scale/rotation alone may not cause a layout-change notification.
    vela_output_manager_update(server);
    return ok;
}

static bool lid_restore_pending(struct vela_server *server, const struct wlr_output_configuration_v1 *config)
{
    if (lid_hides_panels(server, config)) {
        return false;
    }
    struct vela_output *output;
    wl_list_for_each(output, &server->outputs, link)
    {
        if (output->lid_saved.enabled) {
            return true;
        }
    }
    return false;
}

static int refresh_lid(void *data)
{
    struct vela_server *server = data;
    // Keep the source set while committing so layout notifications coalesce.
    struct wlr_output_configuration_v1 *config = snapshot(server, true);
    bool ok = commit_configuration(server, config, false, true);
    bool retry = !ok && lid_restore_pending(server, config);
    wlr_output_configuration_v1_destroy(config);
    if (retry && wl_event_source_timer_update(server->lid_update, 1000) == 0) {
        wlr_log(WLR_DEBUG, "Could not restore the built-in display, retrying");
        return 0;
    }
    if (!ok) {
        wlr_log(WLR_ERROR, "Could not update the built-in display for the lid state");
    }
    wl_event_source_remove(server->lid_update);
    server->lid_update = NULL;
    return 0;
}

void vela_output_manager_schedule_lid_update(struct vela_server *server)
{
    if (server->lid_update) {
        return;
    }
    server->lid_update = VELA_MUST(wl_event_loop_add_timer(server->loop, refresh_lid, server));
    // Defer modesets until the backend has finished notifying us of changes.
    if (wl_event_source_timer_update(server->lid_update, 1) < 0) {
        wlr_log_errno(WLR_ERROR, "Could not schedule a lid display update");
        wl_event_source_remove(server->lid_update);
        server->lid_update = NULL;
    }
}

bool vela_output_manager_save(struct vela_server *server)
{
    if (!server->session) {
        return true; // Non-DRM sessions deliberately do not persist.
    }
    struct wlr_output_configuration_v1 *config = snapshot(server, true);
    int count = wl_list_length(&config->heads);
    char (*keys)[256] = vela_alloc((size_t)count + 1, sizeof(*keys));
    struct vela_output_config_entry *entries = VELA_NEW_N(struct vela_output_config_entry, (size_t)count + 1);
    int n = 0;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each (head, &config->heads, link) {
        const struct wlr_output_head_v1_state *state = &head->state;
        const struct wlr_output *wlr = state->output;
        vela_output_key(wlr, keys[n], sizeof(keys[n]));
        const struct vela_output *output = wlr->data;
        int width = wlr->width, height = wlr->height, refresh = wlr->refresh;
        if (output->lid_saved.enabled) {
            width = state->mode ? state->mode->width : state->custom_mode.width;
            height = state->mode ? state->mode->height : state->custom_mode.height;
            refresh = state->mode ? state->mode->refresh : state->custom_mode.refresh;
        }
        entries[n] = (struct vela_output_config_entry) { keys[n], state->enabled, width, height, refresh, state->scale,
            state->transform, state->x, state->y };
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
    struct wlr_output_configuration_v1 *restore = snapshot(server, true);
    struct wlr_output_configuration_head_v1 *head, *saved;
    wl_list_for_each (head, &restore->heads, link) {
        wl_list_for_each (saved, &before->heads, link) {
            if (head->state.output == saved->state.output) {
                head->state = saved->state;
                break;
            }
        }
    }
    bool ok = commit_configuration(server, restore, true, false);
    wlr_output_configuration_v1_destroy(restore);
    return ok;
}

static bool prepare_client_configuration(struct vela_server *server, struct wlr_output_configuration_v1 *config)
{
    bool hide_panels = lid_hides_panels(server, config);
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
        const struct vela_output *output = head->state.output->data;
        if (!output) {
            return false;
        }
        if (output->lid_saved.enabled && !head->state.enabled) {
            // Clients see a hidden panel as disabled. Keep the previous preference.
            head->state = output->lid_saved;
        } else if (hide_panels && output->internal_panel && head->state.enabled
            && !test_head_state(&head->state, false)) {
            // Check an explicit request as enabled before saving it for later.
            return false;
        }
    }
    return true;
}

static void apply(struct vela_server *server, struct wlr_output_configuration_v1 *config, bool test_only)
{
    bool allowed =
        (test_only || vela_output_preview_can_apply(server, config)) && prepare_client_configuration(server, config);
    struct wlr_output_configuration_v1 *before = !test_only && allowed ? snapshot(server, true) : NULL;
    int origin_x, origin_y;
    configuration_origin(config, &origin_x, &origin_y);
    bool ok = allowed
        && (test_only ? test_configuration(config, lid_hides_panels(server, config), false, origin_x, origin_y)
                      : commit_configuration(server, config, false, false));
    bool restored = true;
    if (!ok && before && !vela_output_manager_restore(server, before)) {
        restored = false;
        wlr_log(WLR_ERROR, "Failed to restore outputs after a rejected configuration");
    }
    bool protected = !test_only && vela_output_preview_applied(server, config, before, ok, restored);
    if (before && !protected) {
        wlr_output_configuration_v1_destroy(before);
    }
    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
        if (!test_only && !protected) {
            if (!vela_output_manager_save(server)) {
                wlr_log(WLR_ERROR, "Could not persist the display configuration");
            }
        }
    } else {
        wlr_output_configuration_v1_send_failed(config);
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
    vela_output_preview_finish(server);
    if (server->lid_update) {
        wl_event_source_remove(server->lid_update);
        server->lid_update = NULL;
    }
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
