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
        if (!head->state.enabled && output->startup_mode) {
            // enable_head inherits these defaults when the client sets only enabled.
            head->state.mode = output->startup_mode;
            head->state.transform = output->startup_transform;
        }
        bool remembered = !head->state.enabled && output->has_last_position;
        head->state.x = remembered ? output->last_x : box.x;
        head->state.y = remembered ? output->last_y : box.y;
    }
    return config;
}

struct wlr_output_configuration_v1 *vela_output_manager_snapshot(struct vela_server *server)
{
    return snapshot(server, false);
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


static bool lid_hides_panels(const struct vela_server *server, const struct wlr_output_configuration_v1 *config)
{
    if (!server->lid_closed) {
        return false;
    }
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link) {
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
    bool valid = wlr_output_test_state(head->output, &state);
    wlr_output_state_finish(&state);
    return valid;
}

static bool head_rect(const struct wlr_output_head_v1_state *state, struct vela_output_rect *rect)
{
    int width = state->mode ? state->mode->width : state->custom_mode.width;
    int height = state->mode ? state->mode->height : state->custom_mode.height;
    bool rotated = state->transform & 1;
    float logical_width = (float)(rotated ? height : width) / state->scale;
    float logical_height = (float)(rotated ? width : height) / state->scale;
    if (!isfinite(logical_width) || logical_width < 1 || (double)logical_width > INT_MAX || !isfinite(logical_height)
        || logical_height < 1 || (double)logical_height > INT_MAX) {
        return false;
    }
    *rect = (struct vela_output_rect) { state->x, state->y, (int)logical_width, (int)logical_height };
    return true;
}

static bool test_configuration(const struct wlr_output_configuration_v1 *config, bool restoring, bool hide_panels)
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
        const struct vela_output *output = wanted->output->data;
        if (!output) {
            valid = false;
            break;
        }
        bool hide = hide_panels && output->internal_panel && wanted->enabled;
        // Never accept a bogus mode merely because the panel is temporarily hidden.
        valid = (!hide || test_head_state(wanted, false)) && test_head_state(wanted, hide);
        if (!valid) {
            break;
        }
        if (!wanted->enabled || hide) {
            continue;
        }
        struct vela_output_rect rect;
        if (!head_rect(wanted, &rect)) {
            valid = false;
            break;
        }
        rect.x -= origin_x;
        rect.y -= origin_y;
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

static void finish_layout_change(
    struct vela_server *server, struct vela_tracked_view *tracked, int n, double pointer_dx, double pointer_dy)
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
        struct vela_view *view = entry->view;
        if (server->cursor_mode == VELA_CURSOR_RESIZE && server->grabbed == view) {
            struct vela_interaction *interaction = server->interaction;
            interaction->grab_box.x += dx;
            interaction->grab_box.y += dy;
            // The pointer can move with a different output than the window.
            interaction->grab_x += pointer_dx - dx;
            interaction->grab_y += pointer_dy - dy;
        }
        if (dx || dy) {
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

static void remember_new_output_position(struct vela_output *output, struct wlr_box box)
{
    // wlroots initially places new outputs to the right of the live layout.
    // Use that neighbor's saved origin when the desktop has been rebased.
    int64_t x = box.x, y = box.y;
    int64_t rightmost = INT64_MIN;
    struct vela_server *server = output->server;
    struct vela_output *neighbor;
    wl_list_for_each(neighbor, &server->outputs, link)
    {
        if (!neighbor->has_preferred_position) {
            continue;
        }
        struct wlr_output_layout_output *layout = wlr_output_layout_get(server->output_layout, neighbor->wlr);
        if (!layout || layout->auto_configured) {
            continue;
        }
        struct wlr_box other = vela_output_box(neighbor);
        int64_t right = (int64_t)other.x + other.width;
        if (right <= box.x && right > rightmost) {
            rightmost = right;
            x = (int64_t)neighbor->preferred_x + box.x - other.x;
            y = (int64_t)neighbor->preferred_y + box.y - other.y;
        }
    }
    output->has_preferred_position = true;
    output->preferred_x = x >= INT_MIN && x <= INT_MAX ? (int)x : box.x;
    output->preferred_y = y >= INT_MIN && y <= INT_MAX ? (int)y : box.y;
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
            remember_new_output_position(output, box);
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
    double old_pointer_x = 0.0, old_pointer_y = 0.0;
    if (moved) {
        // Rescue windows on a removed output before another output takes
        // over its coordinates. Track the surviving windows for translation.
        vela_views_check_outputs(server);
        tracked = track_views(server, &tracked_count);
        if (server->cursor) {
            old_pointer_x = server->cursor->x;
            old_pointer_y = server->cursor->y;
            struct wlr_output *wlr = wlr_output_layout_output_at(server->output_layout, old_pointer_x, old_pointer_y);
            pointed = wlr ? wlr->data : NULL;
            if (pointed) {
                struct wlr_box box = vela_output_box(pointed);
                cursor_x = old_pointer_x - box.x;
                cursor_y = old_pointer_y - box.y;
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
        double pointer_dx = 0, pointer_dy = 0;
        if (pointed) {
            struct wlr_box box = vela_output_box(pointed);
            wlr_cursor_warp_closest(server->cursor, NULL, box.x + cursor_x, box.y + cursor_y);
        }
        if (server->cursor) {
            pointer_dx = server->cursor->x - old_pointer_x;
            pointer_dy = server->cursor->y - old_pointer_y;
        }
        finish_layout_change(server, tracked, tracked_count, pointer_dx, pointer_dy);
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
    if (server->lid_closed) {
        vela_output_manager_schedule_lid_update(server);
    }
    // wlroots removes its layout entry later in the destroy sequence. Wait
    // for that and coalesce hotplug notifications before moving any outputs.
    if (server->output_previews && !server->output_rearrange) {
        server->output_rearrange = VELA_MUST(wl_event_loop_add_idle(server->loop, arrange_after_hotplug, server));
    }
}

static bool commit_configuration(
    struct vela_server *server, const struct wlr_output_configuration_v1 *config, bool restoring, bool lid_only)
{
    // When the lid alone changes, do not modeset or reposition unrelated heads.
    bool hide_panels = lid_hides_panels(server, config);
    if (lid_only && !test_configuration(config, false, hide_panels)) {
        return false;
    }
    int origin_x = 0, origin_y = 0;
    if (!lid_only) {
        configuration_origin(config, &origin_x, &origin_y);
    }
    int n;
    struct vela_tracked_view *tracked = track_views(server, &n);
    bool ok = true, changed = false;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
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
            wlr_output_layout_add(server->output_layout, wanted->output, wanted->x - origin_x, wanted->y - origin_y);
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
    if (changed) {
        finish_layout_change(server, tracked, n, 0, 0);
    } else {
        free(tracked);
    }
    return ok;
}



// Arrange only if a previously hidden panel would otherwise overlap an
// external display that was hotplugged while the lid was closed.
static bool arrange_configuration(struct wlr_output_configuration_v1 *config);

static bool lid_restore_pending(struct vela_server *server, const struct wlr_output_configuration_v1 *config)
{
    if (lid_hides_panels(server, config)) {
        return false;
    }
    struct vela_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->lid_saved.enabled) {
            return true;
        }
    }
    return false;
}

static int refresh_lid(void *data)
{
    struct vela_server *server = data;
    struct wlr_output_configuration_v1 *config = snapshot(server, true);
    bool restoring_panel = lid_restore_pending(server, config);
    bool rearrange = restoring_panel && !test_configuration(config, false, false);
    bool prepared = !rearrange || arrange_configuration(config);
    bool ok = prepared && test_configuration(config, rearrange, lid_hides_panels(server, config))
        && commit_configuration(server, config, rearrange, !rearrange);
    bool retry = !ok && restoring_panel;
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
    if (wl_event_source_timer_update(server->lid_update, 1) < 0) {
        wlr_log_errno(WLR_ERROR, "Could not schedule a lid display update");
        wl_event_source_remove(server->lid_update);
        server->lid_update = NULL;
    }
}

static const struct wlr_output_head_v1_state *find_head(
    const struct wlr_output_configuration_v1 *config, const struct wlr_output *output)
{
    if (!config) {
        return NULL;
    }
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
        if (head->state.output == output) {
            return &head->state;
        }
    }
    return NULL;
}

static bool position_changed(const struct wlr_output_head_v1_state *old, const struct wlr_output_head_v1_state *want)
{
    return !old->enabled || old->x != want->x || old->y != want->y;
}

// Prefer a shared edge with an unchanged head. If there is no shared edge,
// use the nearest unchanged head as the reference for the saved coordinates.
static const struct wlr_output_head_v1_state *preference_anchor(const struct wlr_output_configuration_v1 *before,
    const struct wlr_output_configuration_v1 *config, const struct wlr_output_head_v1_state *want)
{
    struct vela_output_rect rect;
    if (!head_rect(want, &rect)) {
        return NULL;
    }
    const struct wlr_output_head_v1_state *anchor = NULL;
    int64_t best_gap = INT64_MAX, best_shared = 0;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
        const struct wlr_output_head_v1_state *neighbor = &head->state;
        if (!neighbor->enabled || neighbor == want) {
            continue;
        }
        const struct wlr_output_head_v1_state *old = find_head(before, neighbor->output);
        struct vela_output_rect other;
        if (!old || position_changed(old, neighbor) || !head_rect(neighbor, &other)) {
            continue;
        }
        int64_t overlap_x = (rect.x + rect.width < other.x + other.width ? rect.x + rect.width : other.x + other.width)
            - (rect.x > other.x ? rect.x : other.x);
        int64_t overlap_y =
            (rect.y + rect.height < other.y + other.height ? rect.y + rect.height : other.y + other.height)
            - (rect.y > other.y ? rect.y : other.y);
        int64_t gap = (overlap_x < 0 ? -overlap_x : 0) + (overlap_y < 0 ? -overlap_y : 0);
        int64_t shared = overlap_x == 0 && overlap_y > 0 ? overlap_y : overlap_y == 0 && overlap_x > 0 ? overlap_x : 0;
        if (gap < best_gap || (gap == best_gap && shared > best_shared)) {
            anchor = old;
            best_gap = gap;
            best_shared = shared;
        }
    }
    return anchor;
}

static bool output_present(const char *key, void *data)
{
    const struct vela_server *server = data;
    struct vela_output *output;
    wl_list_for_each(output, &server->outputs, link)
    {
        char connected[256];
        vela_output_key(output->wlr, connected, sizeof(connected));
        if (strcmp(connected, key) == 0) {
            return true;
        }
    }
    return false;
}

static bool layout_is_partial(struct vela_server *server, const struct wlr_output_configuration_v1 *before)
{
    bool have_origin = false;
    int64_t origin_x = 0, origin_y = 0;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &before->heads, link)
    {
        const struct vela_output *output = head->state.output->data;
        if (!output->has_preferred_position) {
            continue;
        }
        int64_t x = (int64_t)output->preferred_x - head->state.x;
        int64_t y = (int64_t)output->preferred_y - head->state.y;
        // A common translation leaves the layout intact. Different offsets
        // indicate that at least one head has moved away from its saved slot.
        if (!head->state.enabled || (have_origin && (x != origin_x || y != origin_y))) {
            return true;
        }
        origin_x = x;
        origin_y = y;
        have_origin = true;
    }
    return server->session && vela_output_config_has_absent_output(output_present, server);
}

// Use the original positions for explicit drags and partial layouts. For a
// size change with the full layout present, use the repaired spacing.
static struct wlr_output_configuration_v1 *requested_preferences(struct vela_server *server,
    const struct wlr_output_configuration_v1 *before, const struct wlr_output_configuration_v1 *config,
    const char *implicit)
{
    struct wlr_output_configuration_v1 *preferences = vela_output_manager_snapshot(server);
    const struct wlr_output_head_v1_state *reference = NULL;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &before->heads, link)
    {
        if (head->state.enabled) {
            reference = &head->state;
            break;
        }
    }
    wl_list_for_each(head, &preferences->heads, link)
    {
        const struct wlr_output_head_v1_state *old = find_head(before, head->state.output);
        const struct wlr_output_head_v1_state *want = find_head(config, head->state.output);
        const struct vela_output *output = head->state.output->data;
        if (output->has_preferred_position) {
            head->state.x = output->preferred_x;
            head->state.y = output->preferred_y;
        }
        if (!want || !want->enabled || !position_changed(old, want)
            || (implicit && strcmp(want->output->name, implicit) == 0)) {
            continue;
        }
        int64_t x = want->x, y = want->y;
        const struct wlr_output_head_v1_state *anchor = preference_anchor(before, config, want);
        if (!anchor) {
            anchor = reference;
        }
        const struct vela_output *neighbor = anchor ? anchor->output->data : NULL;
        if (neighbor && neighbor->has_preferred_position) {
            x += (int64_t)neighbor->preferred_x - anchor->x;
            y += (int64_t)neighbor->preferred_y - anchor->y;
        }
        if (x >= INT_MIN && x <= INT_MAX && y >= INT_MIN && y <= INT_MAX) {
            head->state.x = (int)x;
            head->state.y = (int)y;
        }
    }
    return preferences;
}

bool vela_output_manager_save(struct vela_server *server, const struct wlr_output_configuration_v1 *preferences)
{
    struct wlr_output_configuration_v1 *config = snapshot(server, true);
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
        struct vela_output *output = head->state.output->data;
        if (head->state.enabled) {
            output->startup_mode = NULL;
        }
        const struct wlr_output_head_v1_state *preferred = find_head(preferences, output->wlr);
        if (preferred) {
            output->has_preferred_position = true;
            output->preferred_x = preferred->x;
            output->preferred_y = preferred->y;
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
        int x = output->has_preferred_position ? output->preferred_x : state->x;
        int y = output->has_preferred_position ? output->preferred_y : state->y;
        int width = wlr->width, height = wlr->height, refresh = wlr->refresh;
        if (output->lid_saved.enabled) {
            width = state->mode ? state->mode->width : state->custom_mode.width;
            height = state->mode ? state->mode->height : state->custom_mode.height;
            refresh = state->mode ? state->mode->refresh : state->custom_mode.refresh;
        }
        entries[n] = (struct vela_output_config_entry) { keys[n], state->enabled, width, height, refresh,
            state->scale, state->transform, x, y };
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
    bool ok = test_configuration(restore, true, lid_hides_panels(server, restore))
        && commit_configuration(server, restore, true, false);
    wlr_output_configuration_v1_destroy(restore);
    return ok;
}

// Repair geometry inside the preview transaction so Revert restores it all.
static bool arrange_configuration(struct wlr_output_configuration_v1 *config)
{
    size_t count = (size_t)wl_list_length(&config->heads);
    struct wlr_output_head_v1_state **heads = VELA_NEW_N(struct wlr_output_head_v1_state *, count + 1);
    struct vela_output_rect *rects = VELA_NEW_N(struct vela_output_rect, count + 1);
    bool *placed = VELA_NEW_N(bool, count + 1);
    bool ok = true;
    size_t n = 0;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
        if (!head->state.enabled) {
            continue;
        }
        if (!head_rect(&head->state, &rects[n])) {
            ok = false;
            goto done;
        }
        heads[n++] = &head->state;
    }
    if (!n) {
        goto done;
    }
    // Match the hotplug path: anchor the leftmost output.
    size_t anchor = 0;
    for (size_t i = 1; i < n; ++i) {
        if (rects[i].x < rects[anchor].x || (rects[i].x == rects[anchor].x && rects[i].y < rects[anchor].y)) {
            anchor = i;
        }
    }
    struct vela_output_rect first = rects[0];
    rects[0] = rects[anchor];
    rects[anchor] = first;
    struct wlr_output_head_v1_state *first_head = heads[0];
    heads[0] = heads[anchor];
    heads[anchor] = first_head;
    vela_output_arrange_rects(rects, placed, n);
    for (size_t i = 0; i < n; ++i) {
        if (rects[i].x < INT_MIN || rects[i].x > INT_MAX || rects[i].y < INT_MIN || rects[i].y > INT_MAX) {
            ok = false;
            break;
        }
        heads[i]->x = (int)rects[i].x;
        heads[i]->y = (int)rects[i].y;
    }

done:
    free(placed);
    free(rects);
    free(heads);
    return ok;
}

static bool implicit_enable_matches(
    struct vela_server *server, const struct wlr_output_configuration_v1 *config, const char *name)
{
    bool reenabled = false;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
        if (!head->state.enabled) {
            continue;
        }
        struct wlr_output *wlr = head->state.output;
        if (!strcmp(wlr->name, name) && !wlr->enabled) {
            reenabled = true;
        } else {
            struct wlr_box box = { 0 };
            wlr_output_layout_get_box(server->output_layout, wlr, &box);
            if (!wlr->enabled || head->state.x != box.x || head->state.y != box.y) {
                return false; // This request also contains explicit positioning.
            }
        }
    }
    return reenabled;
}

static bool resize_without_moving(struct vela_server *server, const struct wlr_output_configuration_v1 *config)
{
    bool resized = false;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
        const struct wlr_output_head_v1_state *want = &head->state;
        const struct vela_output *output = want->output->data;
        if (!output || output->wlr->enabled != want->enabled) {
            return false;
        }
        if (!want->enabled) {
            continue;
        }
        struct wlr_box current = vela_output_box(output);
        struct vela_output_rect next;
        if (current.x != want->x || current.y != want->y || !head_rect(want, &next)) {
            return false;
        }
        resized |= current.width != next.width || current.height != next.height;
    }
    return resized;
}

static bool prepare_configuration(struct vela_server *server, struct wlr_output_configuration_v1 *config, bool implicit, bool resized)
{
    bool disabled = false;
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link)
    {
        const struct vela_output *output = head->state.output->data;
        if (!output) {
            return false;
        }
        disabled |= output->wlr->enabled && !head->state.enabled;
        if (implicit && head->state.enabled && output->has_preferred_position) {
            // Survivors and the re-enabled head share the saved origin.
            head->state.x = output->preferred_x;
            head->state.y = output->preferred_y;
        }
    }
    if (implicit || resized) {
        return arrange_configuration(config) && test_configuration(config, false, lid_hides_panels(server, config));
    }
    // Reject overlapping explicit positions before repairing a software disable.
    return test_configuration(config, false, lid_hides_panels(server, config))
        && (!disabled || (arrange_configuration(config) && test_configuration(config, false, lid_hides_panels(server, config))));
}


static bool prepare_client_configuration(struct vela_server *server, struct wlr_output_configuration_v1 *config)
{
    bool hide_panels = lid_hides_panels(server, config);
    struct wlr_output_configuration_head_v1 *head;
    wl_list_for_each(head, &config->heads, link) {
        const struct vela_output *output = head->state.output->data;
        if (!output) {
            return false;
        }
        if (output->lid_saved.enabled && !head->state.enabled) {
            // A temporarily disabled panel is not a change to user preferences.
            head->state = output->lid_saved;
        } else if (hide_panels && output->internal_panel && head->state.enabled
            && !test_head_state(&head->state, false)) {
            return false;
        }
    }
    return true;
}

static void apply(struct vela_server *server, struct wlr_output_configuration_v1 *config, bool test_only)
{
    const char *implicit = NULL;
    bool allowed = (test_only || vela_output_preview_can_apply(server, config, &implicit))
        && prepare_client_configuration(server, config);
    if (implicit && !implicit_enable_matches(server, config, implicit)) {
        implicit = NULL;
    }
    struct wlr_output_configuration_v1 *before = !test_only && allowed ? snapshot(server, true) : NULL;
    bool resized = allowed && resize_without_moving(server, config);
    bool save_repaired_positions = before && resized && !layout_is_partial(server, before);
    struct wlr_output_configuration_v1 *preferences =
        before && !save_repaired_positions ? requested_preferences(server, before, config, implicit) : NULL;
    bool prepared = allowed && prepare_configuration(server, config, implicit != NULL, resized);
    if (save_repaired_positions && prepared) {
        preferences = requested_preferences(server, before, config, NULL);
    }
    bool ok = prepared && (test_only || commit_configuration(server, config, false, false));
    bool restored = true;
    if (!ok && prepared && before && !vela_output_manager_restore(server, before)) {
        restored = false;
        wlr_log(WLR_ERROR, "Failed to restore outputs after a rejected configuration");
    }
    bool protected = !test_only && vela_output_preview_applied(server, config, before, preferences, ok, restored);
    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
        if (!test_only && !protected && !vela_output_manager_save(server, preferences)) {
            wlr_log(WLR_ERROR, "Could not persist the display configuration");
        }
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }
    if (before && !protected) {
        if (preferences) {
            wlr_output_configuration_v1_destroy(preferences);
        }
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
    if (server->lid_update) {
        wl_event_source_remove(server->lid_update);
        server->lid_update = NULL;
    }
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
