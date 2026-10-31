// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scene/surface.h"

#include "listen.h"
#include "scene/frame.h"
#include "scene/scene.h"
#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/config.h>
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif
#include <string.h>

static const struct wlr_addon_interface state_addon;

struct vela_surface_state *vela_surface_state_get(struct wlr_surface *surface)
{
    struct wlr_addon *addon = wlr_addon_find(&surface->addons, NULL, &state_addon);
    if (!addon) {
        return NULL;
    }
    struct vela_surface_state *state = wl_container_of(addon, state, addon);
    return state;
}

// From the commit to the damage of the outputs showing it.
static void handle_commit(struct wl_listener *listener, void *data)
{
    struct vela_surface_state *state = wl_container_of(listener, state, commit);
    bool shown = false;
    struct vela_output_frame *frame;
    wl_list_for_each (frame, &state->scene->frames, link) {
        if (vela_output_frame_shows(frame, state->surface)) {
            vela_output_frame_surface_committed(frame, state->surface);
            shown = true;
        }
    }
    // A new or so far hidden surface: maybe it shows now.
    if (!shown) {
        vela_scene_changed(state->scene);
    }
}

static void handle_surface_destroy(struct wlr_addon *addon)
{
    struct vela_surface_state *state = wl_container_of(addon, state, addon);
    struct vela_output_frame *frame;
    wl_list_for_each (frame, &state->scene->frames, link) {
        vela_output_frame_surface_destroyed(frame, state->surface);
    }
    wl_list_remove(&state->commit.link);
    wlr_addon_finish(addon);
    free(state->outputs);
    free(state);
}

static const struct wlr_addon_interface state_addon = {
    .name = "vela-surface-state",
    .destroy = handle_surface_destroy,
};

void vela_surface_state_create(struct vela_scene *scene, struct wlr_surface *surface)
{
    struct vela_surface_state *state = calloc(1, sizeof(*state));
    state->surface = surface;
    state->scene = scene;
    vela_listen(&surface->events.commit, &state->commit, handle_commit);
    wlr_addon_init(&state->addon, &surface->addons, NULL, &state_addon);
}

static bool on_output(const struct vela_surface_state *state, struct wlr_output *output)
{
    for (int i = 0; i < state->output_count; ++i) {
        if (state->outputs[i].output == output && state->outputs[i].overlap > 0) {
            return true;
        }
    }
    return false;
}

static void refresh(struct vela_surface_state *state)
{
    struct wlr_output *primary = NULL;
    struct wlr_output *pacing = NULL;
    int64_t best_overlap = 0;
    int64_t best_visible = 0;
    for (int i = 0; i < state->output_count; ++i) {
        const struct vela_surface_on_output *on = &state->outputs[i];
        if (on->overlap > best_overlap) {
            best_overlap = on->overlap;
            primary = on->output;
        }
        if (on->visible > best_visible
            || (on->visible > 0 && on->visible == best_visible && pacing && on->output->refresh > pacing->refresh)) {
            best_visible = on->visible;
            pacing = on->output;
        }
    }
    state->pacing = pacing;
    if (!primary) {
        // It isn't visible anywhere (minimized, off screen): no leave, so that
        // showing up on the same output again changes nothing for the app.
        return;
    }

    struct wlr_surface *surface = state->surface;
    struct wlr_surface_output *entered, *tmp;
    wl_list_for_each_safe (entered, tmp, &surface->current_outputs, link) {
        if (!on_output(state, entered->output)) {
            wlr_surface_send_leave(surface, entered->output);
        }
    }
    for (int i = 0; i < state->output_count; ++i) {
        if (state->outputs[i].overlap > 0) {
            wlr_surface_send_enter(surface, state->outputs[i].output);
        }
    }
    // The scale of the output showing the largest part (§3.6).
    double scale = primary->scale;
#if WLR_HAS_XWAYLAND
    // KWin-style experimental X11 virtual output geometry is already scaled
    // by the global X11 factor. Do not ALSO suggest fractional/ceil(1.25)=2
    // buffer scaling to XWayland: that can leave the right/bottom of the
    // screen unpainted and mismatch mouse hit testing.
    if (state->scene->x11_scale_override) {
        struct wlr_surface *root = wlr_surface_get_root_surface(surface);
        if (root && wlr_xwayland_surface_try_from_wlr_surface(root)) {
            scale = 1.0;
        }
    }
#endif
    wlr_fractional_scale_v1_notify_scale(surface, scale);
    wlr_surface_set_preferred_buffer_scale(surface, (int32_t)ceil(scale));
}

void vela_surface_report(struct wlr_surface *surface, struct wlr_output *output, int64_t overlap, int64_t visible)
{
    struct vela_surface_state *state = vela_surface_state_get(surface);
    if (!state) {
        return;
    }
    for (int i = 0; i < state->output_count; ++i) {
        struct vela_surface_on_output *on = &state->outputs[i];
        if (on->output != output) {
            continue;
        }
        if (on->overlap == overlap && on->visible == visible) {
            return;
        }
        on->overlap = overlap;
        on->visible = visible;
        refresh(state);
        return;
    }
    state->outputs = vela_grow(state->outputs, &state->output_capacity, state->output_count + 1,
        sizeof(*state->outputs));
    state->outputs[state->output_count++] = (struct vela_surface_on_output) { output, overlap, visible };
    refresh(state);
}

void vela_surface_forget(struct wlr_surface *surface, struct wlr_output *output)
{
    struct vela_surface_state *state = vela_surface_state_get(surface);
    if (!state) {
        return;
    }
    int kept = 0;
    for (int i = 0; i < state->output_count; ++i) {
        if (state->outputs[i].output != output) {
            state->outputs[kept++] = state->outputs[i];
        }
    }
    if (kept != state->output_count) {
        state->output_count = kept;
        refresh(state);
    }
}
