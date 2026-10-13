// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output.h"

#include "config.h"
#include "geometry.h"
#include "layer.h"
#include "listen.h"
#include "nested.h"
#include "output_config.h"
#include "output_key.h"
#include "output_manager.h"
#include "render/renderer.h"
#include "scene/frame.h"
#include "scene/scene.h"
#include "server.h"
#include "util.h"
#include "view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

// ---------------------------------------------------------------- creation --

// Many high refresh rate monitors declare a 60 Hz mode as "preferred". We take
// the preferred resolution at the highest refresh rate available: 180 Hz on a
// 180 Hz monitor without touching anything.
static struct wlr_output_mode *pick_mode(struct wlr_output *output)
{
    struct wlr_output_mode *preferred = wlr_output_preferred_mode(output);
    if (!preferred) {
        return NULL; // such as the nested backend: no mode list
    }
    struct wlr_output_mode *best = preferred;
    struct wlr_output_mode *mode;
    wl_list_for_each (mode, &output->modes, link) {
        if (mode->width == preferred->width && mode->height == preferred->height && mode->refresh > best->refresh) {
            best = mode;
        }
    }
    return best;
}

// The output mode closest to the saved one, NULL if there is none.
static struct wlr_output_mode *find_mode(struct wlr_output *output, int width, int height, int refresh_mhz)
{
    struct wlr_output_mode *best = NULL;
    struct wlr_output_mode *mode;
    wl_list_for_each (mode, &output->modes, link) {
        if (mode->width != width || mode->height != height) {
            continue;
        }
        if (!best || abs(mode->refresh - refresh_mhz) < abs(best->refresh - refresh_mhz)) {
            best = mode;
        }
    }
    return best;
}

void vela_output_key(const struct wlr_output *output, char *out, size_t size)
{
    vela_output_key_format(out, size, output->make, output->model, output->serial, output->name);
}

// The initial state: what the user chose last time for this monitor (only in
// the real session), otherwise the best mode and the scale from DPI. `saved`
// is NULL when nothing was saved.
static void initial_state(struct wlr_output *wlr, const struct vela_saved_output *saved,
    struct wlr_output_state *state)
{
    bool enabled = !saved || saved->enabled;
    wlr_output_state_set_enabled(state, enabled);
    struct wlr_output_mode *saved_mode
        = saved && saved->width > 0 ? find_mode(wlr, saved->width, saved->height, saved->refresh_mhz) : NULL;
    struct wlr_output_mode *best = NULL;
    const char *size = getenv("VELA_OUTPUT_SIZE");
    if (!enabled) {
        // Off, as the user left it.
    } else if (saved_mode) {
        wlr_output_state_set_mode(state, saved_mode);
    } else if ((best = pick_mode(wlr))) {
        wlr_output_state_set_mode(state, best);
    } else if (size) {
        // An output without modes (nested window, headless): we choose the
        // size, such as VELA_OUTPUT_SIZE=1920x1080, with an optional refresh
        // rate for tests: 1920x1080@144.
        int width = 0;
        int height = 0;
        double hz = 0.0;
        int fields = sscanf(size, "%dx%d@%lf", &width, &height, &hz);
        if (fields >= 2 && width > 0 && height > 0) {
            wlr_output_state_set_custom_mode(state, width, height, fields == 3 ? (int32_t)(hz * 1000.0) : 0);
        }
    }
    if (saved && enabled) {
        wlr_output_state_set_transform(state, (enum wl_output_transform)saved->transform);
    }
    float requested = vela_requested_scale(getenv("VELA_SCALE"), wlr->name);
    if (requested > 0.0f) {
        wlr_output_state_set_scale(state, requested);
    } else if (saved && saved->scale > 0.0f) {
        wlr_output_state_set_scale(state, saved->scale);
    } else if (state->committed & WLR_OUTPUT_STATE_MODE) {
        bool fixed = state->mode_type == WLR_OUTPUT_STATE_MODE_FIXED;
        int width = fixed ? state->mode->width : state->custom_mode.width;
        int height = fixed ? state->mode->height : state->custom_mode.height;
        double dpi = 0.0;
        float scale = vela_scale_for_dpi(wlr->phys_width, wlr->phys_height, width, height,
            vela_is_internal_panel(wlr->name), &dpi);
        wlr_output_state_set_scale(state, scale);
        if (dpi > 0.0) {
            wlr_log(WLR_INFO, "%s: %.0f DPI (%d×%d mm), default scale %.0f%%", wlr->name, dpi, wlr->phys_width,
                wlr->phys_height, scale * 100.0);
        }
    }
}

static int handle_latch_timer(int fd, uint32_t mask, void *data);
void vela_server_schedule_frames(struct vela_server *server)
{
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        vela_output_schedule_frame(output);
    }
}

void vela_output_load_settings(struct vela_server *server)
{
    struct vela_config settings;
    vela_config_read(&settings);
    const char *vrr = vela_config_get(&settings, "variable-refresh", "games");
    server->vrr_mode = strcmp(vrr, "always") == 0 ? 2 : strcmp(vrr, "no") == 0 ? 0 : 1;
    const char *env = getenv("VELA_VRR");
    if (env && *env) {
        server->vrr_mode = strcmp(env, "0") == 0 ? 0 : strcmp(env, "1") == 0 ? 2 : server->vrr_mode;
    }
    vela_server_schedule_frames(server);
    server->scene->allow_tearing = vela_config_flag(&settings, "tearing", true) && !vela_env_off("VELA_TEARING");
    vela_config_finish(&settings);
}

static void handle_frame_event(struct wl_listener *listener, void *data);
static void handle_present(struct wl_listener *listener, void *data);
static void handle_request_state(struct wl_listener *listener, void *data);
static void handle_destroy(struct wl_listener *listener, void *data);
static void start_virtual_vblank(struct vela_output *output);

void vela_output_create(struct vela_server *server, struct wlr_output *wlr)
{
    struct vela_output *output = calloc(1, sizeof(*output));
    output->server = server;
    output->wlr = wlr;
    output->powered = true;
    output->latch_fd = -1;
    output->vblank_fd = -1;
    wl_list_init(&output->link);
    wlr->data = output;
    // Everything drawn on this output goes through Vela's renderer, including
    // what wlroots does (cursor, captures).
    wlr_output_init_render(wlr, server->allocator, server->wlr_renderer);

    char key[256];
    vela_output_key(wlr, key, sizeof(key));
    struct vela_saved_output saved_output;
    const struct vela_saved_output *saved
        = server->session && vela_output_config_load(key, &saved_output) ? &saved_output : NULL;
    bool enabled = !saved || saved->enabled;
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    initial_state(wlr, saved, &state);
    // The frame cycle turns VRR on and off (vrr_state).
    wlr_output_commit_state(wlr, &state);
    wlr_output_state_finish(&state);
    if (enabled) {
        wlr_log(WLR_INFO, "Output %s: %dx%d @ %.2f Hz, scale %.2f%s", wlr->name, wlr->width, wlr->height,
            wlr->refresh / 1000.0, wlr->scale, saved ? " (as saved)" : "");
    } else {
        wlr_log(WLR_INFO, "Output %s: off, as saved", wlr->name);
    }

    output->frame = vela_output_frame_create(server->scene, server->renderer, wlr, output);
    vela_frame_clock_init(&output->clock);
    vela_frame_clock_set_mode_refresh(&output->clock, wlr->refresh);
    // Late latching (§4.3): VELA_LATCH=0 turns it off (drawing starts as soon
    // as the backend says "frame"); VELA_LATCH_MARGIN: minimum margin in ms.
    output->latching = !vela_env_off("VELA_LATCH");
    const char *margin = getenv("VELA_LATCH_MARGIN");
    if (margin && *margin) {
        vela_frame_clock_set_base_margin(&output->clock, (int64_t)(strtod(margin, NULL) * 1e6));
    }
    output->latch_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (output->latch_fd >= 0) {
        output->latch_source = wl_event_loop_add_fd(server->loop, output->latch_fd, WL_EVENT_READABLE,
            handle_latch_timer, output);
    } else {
        output->latching = false;
    }
    wl_list_init(&output->present.link);
    wl_list_init(&output->frame_event.link);
    if (wlr_output_is_headless(wlr)) {
        start_virtual_vblank(output);
    } else {
        vela_listen(&wlr->events.present, &output->present, handle_present);
        vela_listen(&wlr->events.frame, &output->frame_event, handle_frame_event);
    }
    vela_listen(&wlr->events.request_state, &output->request_state, handle_request_state);
    vela_listen(&wlr->events.destroy, &output->destroy, handle_destroy);

    // In the list before entering the layout: the layout change notifies the
    // output configuration programs, and this output must already be there.
    wl_list_insert(server->outputs.prev, &output->link);
    if (!enabled) {
        // Out of the layout until someone turns it back on.
        vela_output_manager_update(server);
    } else if (saved && saved->has_position) {
        wlr_output_layout_add(server->output_layout, wlr, saved->x, saved->y);
    } else {
        wlr_output_layout_add_auto(server->output_layout, wlr);
    }
    if (wlr_output_is_wl(wlr)) {
        output->nested = vela_nested_create(output);
    }
    output->usable = vela_output_box(output);
    vela_output_schedule_frame(output);
}

static void handle_destroy(struct wl_listener *listener, void *data)
{
    struct vela_output *output = wl_container_of(listener, output, destroy);
    struct vela_server *server = output->server;
    // Panels closed, snap preview and magnifier off, the lock no longer waits
    // for black on this output.
    vela_server_output_destroyed(server, output);
    wl_list_remove(&output->link);
    vela_output_manager_update(server);
    output->wlr->data = NULL;
    wl_list_remove(&output->frame_event.link);
    wl_list_remove(&output->present.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    if (output->vblank_source) {
        wl_event_source_remove(output->vblank_source);
    }
    if (output->vblank_fd >= 0) {
        close(output->vblank_fd);
    }
    if (output->idle_frame) {
        wl_event_source_remove(output->idle_frame);
    }
    if (output->latch_source) {
        wl_event_source_remove(output->latch_source);
    }
    if (output->latch_fd >= 0) {
        close(output->latch_fd);
    }
    vela_nested_destroy(output->nested);
    vela_output_frame_destroy(output->frame);
    free(output);
}

// In the nested backend: the host window was resized.
static void handle_request_state(struct wl_listener *listener, void *data)
{
    struct vela_output *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    const struct wlr_output_state *requested = event->state;
    if (output->nested && (requested->committed & WLR_OUTPUT_STATE_MODE)
        && requested->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
        vela_nested_resize(output->nested, requested->custom_mode.width, requested->custom_mode.height);
        return;
    }
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_copy(&state, requested);
    vela_output_commit_mode(output, &state);
    wlr_output_state_finish(&state);
}

bool vela_output_commit_mode(struct vela_output *output, struct wlr_output_state *state)
{
    struct wlr_box area = vela_output_box(output);
    bool ok = vela_output_frame_render(output->frame, area.x, area.y, state);
    if (!ok) {
        // Fallback (also to turn the output on or off): wlroots puts an empty
        // buffer, which our damage tracking doesn't know about.
        ok = wlr_output_commit_state(output->wlr, state);
        vela_output_frame_reset_damage(output->frame);
    }
    vela_frame_clock_set_mode_refresh(&output->clock, output->wlr->refresh);
    vela_output_arrange_layers(output);
    if (ok && (state->committed & (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_ENABLED))) {
        // The DRM gamma capabilities may have changed with the mode/power state.
        // Retry after the mode commit, not while testing the old output state.
        vela_output_frame_retry_night_light(output->frame);
    }
    vela_output_schedule_frame(output);
    return ok;
}

void vela_output_set_powered(struct vela_output *output, bool on)
{
    if (on == output->powered) {
        return;
    }
    struct wlr_output *wlr = output->wlr;
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    if (!on) {
        output->mode_before_off = wlr->current_mode;
        output->custom_before_off[0] = wlr->width;
        output->custom_before_off[1] = wlr->height;
        output->custom_before_off[2] = wlr->refresh;
        wlr_output_state_set_enabled(&state, false);
        wlr_output_commit_state(wlr, &state);
    } else {
        wlr_output_state_set_enabled(&state, true);
        if (output->mode_before_off) {
            wlr_output_state_set_mode(&state, output->mode_before_off);
        } else if (output->custom_before_off[0] > 0) {
            wlr_output_state_set_custom_mode(&state, output->custom_before_off[0], output->custom_before_off[1],
                output->custom_before_off[2]);
        }
        vela_output_commit_mode(output, &state);
    }
    wlr_output_state_finish(&state);
    output->powered = on;
    wlr_log(WLR_INFO, "%s: screen %s", wlr->name, on ? "back on" : "off because idle");
}

// --------------------------------------------------------- the frame cycle --
// The backend says "frame" (at the vblank after a delivery, or at once if the
// output was idle), drawing is planned as late as possible before the vblank,
// and at the right moment it draws (docs/renderer.md §4.3).

static void on_frame_event(struct vela_output *output);
static void on_frame(struct vela_output *output);
static void arm_virtual_vblank(struct vela_output *output);
static void collect_costs(struct vela_output *output);

static void handle_idle_frame(void *data)
{
    struct vela_output *output = data;
    output->idle_frame = NULL;
    on_frame_event(output);
}

void vela_output_schedule_frame(struct vela_output *output)
{
    output->frame_requested = true;
    // Like wlroots with DRM: if no delivered frame is still waiting for the
    // vblank, "frame" comes at once; otherwise with the page flip.
    // wlr_output_schedule_frame isn't used: it would mark the output as
    // needing a commit even when nothing on it changes (an empty commit, which
    // with DRM blocks until the vblank).
    bool waiting = output->vblank_fd >= 0 ? output->awaiting_present : output->wlr->frame_pending;
    if (!waiting && !output->latch_armed && !output->idle_frame) {
        output->idle_frame = wl_event_loop_add_idle(output->server->loop, handle_idle_frame, output);
    }
}

static void handle_frame_event(struct wl_listener *listener, void *data)
{
    struct vela_output *output = wl_container_of(listener, output, frame_event);
    on_frame_event(output);
}

static void handle_present(struct wl_listener *listener, void *data)
{
    struct vela_output *output = wl_container_of(listener, output, present);
    const struct wlr_output_event_present *event = data;
    if (event->presented) {
        vela_frame_clock_presented(&output->clock, event->commit_seq, vela_timespec_ns(&event->when), event->refresh);
    }
    collect_costs(output);
}

static void arm_latch(struct vela_output *output, int64_t when)
{
    const struct itimerspec spec = { .it_value = vela_ns_timespec(when) };
    timerfd_settime(output->latch_fd, TFD_TIMER_ABSTIME, &spec, NULL);
    output->latch_armed = true;
}

static int handle_latch_timer(int fd, uint32_t mask, void *data)
{
    struct vela_output *output = data;
    uint64_t expirations = 0;
    if (read(fd, &expirations, sizeof(expirations)) < 0) {
        return 0;
    }
    output->latch_armed = false;
    on_frame(output);
    return 0;
}

static void on_frame_event(struct vela_output *output)
{
    if (output->latch_armed || (!output->frame_requested && !output->wlr->needs_frame)) {
        return; // already planned, or nothing to do
    }
    collect_costs(output);
    // Tearing (a fullscreen game asking for it), or VRR with a game in direct
    // scanout: its frame goes on screen as soon as it arrives, not at the best
    // moment before a vblank that with VRR isn't fixed.
    const struct vela_frame_delivered *last = &output->frame->delivered;
    if (last->tearing || (last->scanout && output->wlr->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED)) {
        output->planned = false;
        on_frame(output);
        return;
    }
    int64_t now = vela_now_ns();
    output->plan = vela_frame_clock_plan(&output->clock, now, output->latching);
    output->planned = true;
    if (output->plan.start - now > 50000) {
        arm_latch(output, output->plan.start);
        return;
    }
    on_frame(output);
}

// VRR according to the user's choice (vela_server.vrr_mode): if it must be
// turned on or off it goes into `state` and true is returned. "games": only
// with a fullscreen app on this output (like KWin's "Automatic": the desktop
// doesn't flicker on monitors that do); "always"; "no".
static bool vrr_state(struct vela_output *output, struct wlr_output_state *state)
{
    struct vela_server *server = output->server;
    bool wanted = server->vrr_mode == 2 || (server->vrr_mode == 1 && vela_views_fullscreen_on(server, output));
    bool enabled = output->wlr->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
    if (wanted == enabled || (wanted && output->vrr_unsupported)) {
        return false;
    }
    wlr_output_state_set_adaptive_sync_enabled(state, wanted);
    if (!wlr_output_test_state(output->wlr, state)) {
        if (wanted) {
            wlr_log(WLR_INFO, "%s: VRR not supported", output->wlr->name);
            output->vrr_unsupported = true;
        }
        state->committed &= ~WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
        return false;
    }
    wlr_log(WLR_INFO, "%s: VRR %s", output->wlr->name, wanted ? "on" : "off");
    return true;
}

static void log_stats(struct vela_output *output, int64_t now)
{
    // VELA_STATS=1: statistics even without the verbose log.
    bool requested = vela_env_flag("VELA_STATS");
    struct vela_frame_stats stats;
    if (!vela_frame_clock_take_stats(&output->clock, now, &stats) || stats.fps <= 0.0) {
        return;
    }
    wlr_log(requested ? WLR_INFO : WLR_DEBUG,
        "%s: %.2f fps (period %.3f ms, latency %d vblank); cost %.3f ms + margin %.3f ms; "
        "draw to light %.3f ms; prediction error mean %.3f ms, max %.3f ms; missed vblanks %d",
        output->wlr->name, stats.fps, vela_frame_clock_period(&output->clock) / 1e6, output->clock.latency_frames,
        stats.cost_ms, stats.margin_ms, stats.latency_ms, stats.error_mean_ms, stats.error_max_ms, stats.missed);
    if (requested && output->breakdown_count > 0) {
        const double n = output->breakdown_count;
        const double *sum = output->breakdown_sum;
        const double *max = output->breakdown_max;
        wlr_log(WLR_INFO,
            "%s: mean (max) late wakeup %.3f (%.3f) ms, CPU %.3f (%.3f) ms, "
            "GPU wait %.3f (%.3f) ms, GPU work %.3f (%.3f) ms",
            output->wlr->name, sum[0] / n, max[0], sum[1] / n, max[1], sum[2] / n, max[2], sum[3] / n, max[3]);
    }
    memset(output->breakdown_sum, 0, sizeof(output->breakdown_sum));
    memset(output->breakdown_max, 0, sizeof(output->breakdown_max));
    output->breakdown_count = 0;
}

static void on_frame(struct vela_output *output)
{
    // A page flip still in progress (such as after a mode change): its "frame"
    // event will call us back.
    if (output->vblank_fd < 0 && output->wlr->frame_pending) {
        output->planned = false;
        return;
    }
    int64_t now = vela_now_ns();
    struct vela_frame_plan plan = output->planned ? output->plan : vela_frame_clock_plan(&output->clock, now, false);
    output->planned = false;
    output->frame_requested = false;

    // First every animation advances to the moment this frame will become
    // light (docs/renderer.md §4.2), then it draws.
    vela_server_animate(output->server, plan.present);

    struct wlr_box area = vela_output_box(output);
    // VRR to turn on or off goes into this frame's commit.
    struct wlr_output_state vrr;
    wlr_output_state_init(&vrr);
    bool vrr_change = vrr_state(output, &vrr);
    bool rendered = vela_output_frame_render(output->frame, area.x, area.y, vrr_change ? &vrr : NULL);
    wlr_output_state_finish(&vrr);
    if (rendered) {
        vela_server_output_rendered(output->server, output);
        const struct vela_frame_delivered *delivered = &output->frame->delivered;
        struct vela_delivery delivery = {
            output->wlr->commit_seq, plan.start, now, vela_now_ns(), delivered->point, delivered->timing_slot,
        };
        vela_frame_clock_committed(&output->clock, output->wlr->commit_seq, plan.start, plan.present);
        if (output->delivery_count == VELA_OUTPUT_DELIVERIES) {
            memmove(&output->deliveries[0], &output->deliveries[1],
                (VELA_OUTPUT_DELIVERIES - 1) * sizeof(output->deliveries[0]));
            --output->delivery_count;
        }
        output->deliveries[output->delivery_count++] = delivery;
        // With the virtual vblank the frame shows at the first beat when it's
        // ready.
        if (output->vblank_fd >= 0) {
            output->awaiting = delivery;
            output->awaiting_present = true;
            arm_virtual_vblank(output);
        }
    }

    struct timespec when;
    clock_gettime(CLOCK_MONOTONIC, &when);
    vela_output_frame_send_frame_done(output->frame, &when);
    log_stats(output, now);
}

// When the frame was ready: commit done and GPU finished. false if not known
// yet.
static bool ready_time(struct vela_output *output, const struct vela_delivery *delivery, int64_t *when)
{
    if (delivery->point == 0) {
        *when = delivery->committed_at;
        return true;
    }
    struct vela_renderer *renderer = output->server->renderer;
    struct vela_gpu_timing timing;
    if (vela_renderer_read_timing(renderer, delivery->timing_slot, delivery->point, &timing)) {
        // Without calibrated timestamps only how long the GPU worked is known:
        // it's assumed to have started at the commit.
        *when = timing.absolute ? (delivery->committed_at > timing.end_ns ? delivery->committed_at : timing.end_ns)
                                : delivery->committed_at + timing.end_ns;
        return true;
    }
    if (delivery->point <= vela_renderer_completed(renderer)) {
        *when = delivery->committed_at; // finished, but not measured
        return true;
    }
    return false;
}

static void breakdown_add(struct vela_output *output, int i, double ms)
{
    output->breakdown_sum[i] += ms;
    if (ms > output->breakdown_max[i]) {
        output->breakdown_max[i] = ms;
    }
}

// The cost of frames already delivered, as soon as the GPU has finished them.
static void collect_costs(struct vela_output *output)
{
    int64_t now = vela_now_ns();
    int kept = 0;
    for (int i = 0; i < output->delivery_count; ++i) {
        const struct vela_delivery *delivery = &output->deliveries[i];
        int64_t ready = 0;
        if (ready_time(output, delivery, &ready)) {
            vela_frame_clock_add_cost(&output->clock, now, ready - delivery->start);
            breakdown_add(output, 0, (double)(delivery->woke_at - delivery->start) / 1e6);
            breakdown_add(output, 1, (double)(delivery->committed_at - delivery->woke_at) / 1e6);
            struct vela_gpu_timing timing;
            if (delivery->point
                && vela_renderer_read_timing(output->server->renderer, delivery->timing_slot, delivery->point, &timing)
                && timing.absolute) {
                breakdown_add(output, 2, (double)(timing.start_ns - delivery->committed_at) / 1e6);
                breakdown_add(output, 3, (double)(timing.end_ns - timing.start_ns) / 1e6);
            }
            ++output->breakdown_count;
            continue;
        }
        if (now - delivery->committed_at > VELA_NS_PER_SEC) {
            continue; // measurement lost
        }
        output->deliveries[kept++] = *delivery;
    }
    output->delivery_count = kept;
}

// ------------------------------------------------------------ virtual vblank --
// Headless output: a virtual vblank exact to the nanosecond, to test any
// refresh rate (the wlroots headless backend timer works in milliseconds and
// has no vblank). It beats only when a delivered frame is waiting to show; a
// frame shows at the first vblank when it's ready (the GPU must have finished
// too), as on a real output.

static int handle_virtual_vblank(int fd, uint32_t mask, void *data);

static void start_virtual_vblank(struct vela_output *output)
{
    output->vblank_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (output->vblank_fd < 0) {
        wlr_log_errno(WLR_ERROR, "%s: timerfd for the virtual vblank", output->wlr->name);
        return;
    }
    output->vblank_source = wl_event_loop_add_fd(output->server->loop, output->vblank_fd, WL_EVENT_READABLE,
        handle_virtual_vblank, output);
    output->last_vblank = vela_now_ns();
    wlr_log(WLR_INFO, "%s: virtual vblank every %.3f ms", output->wlr->name,
        vela_frame_clock_period(&output->clock) / 1e6);
}

// The next vblank on the exact grid (last vblank + multiples of the period),
// even if the timer stayed idle for long.
static void arm_virtual_vblank(struct vela_output *output)
{
    if (output->vblank_armed || output->vblank_fd < 0) {
        return;
    }
    int64_t period = vela_frame_clock_period(&output->clock);
    int64_t now = vela_now_ns();
    int64_t next = output->last_vblank + period;
    if (next <= now) {
        next = output->last_vblank + ((now - output->last_vblank) / period + 1) * period;
    }
    const struct itimerspec when = { .it_value = vela_ns_timespec(next) };
    timerfd_settime(output->vblank_fd, TFD_TIMER_ABSTIME, &when, NULL);
    output->vblank_armed = true;
}

static int handle_virtual_vblank(int fd, uint32_t mask, void *data)
{
    struct vela_output *output = data;
    uint64_t expirations = 0;
    if (read(fd, &expirations, sizeof(expirations)) < 0) {
        return 0;
    }
    output->vblank_armed = false;
    int64_t period = vela_frame_clock_period(&output->clock);
    int64_t now = vela_now_ns();
    // The last vblank of the grid that has passed (the timer can wake us
    // late).
    int64_t anchor = output->last_vblank;
    int64_t steps = (now - anchor) / period;
    int64_t vblank = anchor + (steps > 1 ? steps : 1) * period;
    output->last_vblank = vblank;

    if (output->awaiting_present) {
        // The frame shows at the first vblank when it was ready: commit done
        // and GPU finished. If it isn't yet, the previous one stays on screen
        // and we try again at the next beat (missed vblank).
        int64_t ready = 0;
        if (!ready_time(output, &output->awaiting, &ready)) {
            arm_virtual_vblank(output);
            return 0;
        }
        int64_t late = ready - anchor > 0 ? ready - anchor : 0;
        int64_t periods = (late + period - 1) / period;
        int64_t shown_at = anchor + (periods > 1 ? periods : 1) * period;
        if (shown_at > vblank) {
            arm_virtual_vblank(output);
            return 0;
        }
        vela_frame_clock_presented(&output->clock, output->awaiting.seq, shown_at, period);
        output->awaiting_present = false;
        collect_costs(output);
    }
    // Like DRM's "frame" event at the vblank after a delivery.
    on_frame_event(output);
    return 0;
}

// ------------------------------------------------ areas in physical pixels --

struct wlr_box vela_output_box(const struct vela_output *output)
{
    struct wlr_box box = { 0 };
    wlr_output_layout_get_box(output->server->output_layout, output->wlr, &box);
    return box;
}

struct vela_area vela_output_from_physical(const struct vela_output *output, struct wlr_box physical)
{
    struct wlr_box full = vela_output_box(output);
    double scale = output->wlr->scale;
    return (struct vela_area) { full.x + physical.x / scale, full.y + physical.y / scale, physical.width / scale,
        physical.height / scale };
}

struct vela_area vela_output_full_area(const struct vela_output *output)
{
    int width = 0;
    int height = 0;
    wlr_output_transformed_resolution(output->wlr, &width, &height);
    return vela_output_from_physical(output, (struct wlr_box) { 0, 0, width, height });
}

// The area free of panels in output pixels: edges touching the output's stay
// exactly on its pixels.
struct wlr_box vela_output_physical_usable(const struct vela_output *output)
{
    struct wlr_box full = vela_output_box(output);
    const struct wlr_box *usable = &output->usable;
    int width = 0;
    int height = 0;
    wlr_output_transformed_resolution(output->wlr, &width, &height);
    double scale = output->wlr->scale;
    int x1 = vela_physical_edge(usable->x, full.x, full.x + full.width, width, scale);
    int x2 = vela_physical_edge(usable->x + usable->width, full.x, full.x + full.width, width, scale);
    int y1 = vela_physical_edge(usable->y, full.y, full.y + full.height, height, scale);
    int y2 = vela_physical_edge(usable->y + usable->height, full.y, full.y + full.height, height, scale);
    return (struct wlr_box) { x1, y1, x2 > x1 ? x2 - x1 : 0, y2 > y1 ? y2 - y1 : 0 };
}

struct vela_area vela_output_usable_area(const struct vela_output *output)
{
    return vela_output_from_physical(output, vela_output_physical_usable(output));
}

// An output edge without another output next to it: what spills over there
// isn't seen.
static bool open_at(const struct vela_output *output, double lx, double ly)
{
    return !wlr_output_layout_output_at(output->server->output_layout, lx, ly);
}

// The client draws a buffer of round(size × scale) pixels
// (fractional-scale-v1, scale in 120ths): for each axis we look for the
// logical size that gives exactly the area's pixels.
struct vela_placement vela_output_place(const struct vela_output *output, struct vela_area area)
{
    struct wlr_box full = vela_output_box(output);
    int screen_width = 0;
    int screen_height = 0;
    wlr_output_transformed_resolution(output->wlr, &screen_width, &screen_height);
    double scale = output->wlr->scale;
    double mid_x = full.x + full.width / 2.0;
    double mid_y = full.y + full.height / 2.0;

    int px = (int)lround((area.x - full.x) * scale);
    int py = (int)lround((area.y - full.y) * scale);
    int pw = (int)lround(area.width * scale);
    int ph = (int)lround(area.height * scale);
    struct vela_axis x = vela_place_axis(px, pw, screen_width, scale, open_at(output, full.x - 0.5, mid_y),
        open_at(output, full.x + full.width + 0.5, mid_y));
    struct vela_axis y = vela_place_axis(py, ph, screen_height, scale, open_at(output, mid_x, full.y - 0.5),
        open_at(output, mid_x, full.y + full.height + 0.5));
    return (struct vela_placement) { full.x + x.offset, full.y + y.offset, x.size, y.size };
}

void vela_output_arrange_layers(struct vela_output *output)
{
    struct wlr_box full = vela_output_box(output);
    struct wlr_box area = full;
    vela_layers_configure(output->server, output, &full, &area);
    bool changed = area.x != output->usable.x || area.y != output->usable.y || area.width != output->usable.width
        || area.height != output->usable.height;
    output->usable = area;
    if (changed) {
        vela_views_usable_changed(output->server, output);
    }
}

struct vela_output *vela_output_at(const struct vela_server *server, double lx, double ly)
{
    struct wlr_output *wlr = wlr_output_layout_output_at(server->output_layout, lx, ly);
    return wlr ? wlr->data : NULL;
}

struct vela_output *vela_output_named(const struct vela_server *server, const char *name)
{
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        if (name && strcmp(output->wlr->name, name) == 0) {
            return output;
        }
    }
    return NULL;
}

struct wlr_box vela_output_fit(const struct vela_output *output, struct wlr_box frame)
{
    const struct wlr_box *area = &output->usable;
    frame.width = vela_min(frame.width, area->width);
    frame.height = vela_min(frame.height, area->height);
    frame.x = vela_clamp(frame.x, area->x, area->x + area->width - frame.width);
    frame.y = vela_clamp(frame.y, area->y, area->y + area->height - frame.height);
    return frame;
}

struct vela_output *vela_output_under_cursor(const struct vela_server *server)
{
    struct vela_output *output = vela_output_at(server, server->cursor->x, server->cursor->y);
    if (output || wl_list_empty(&server->outputs)) {
        return output;
    }
    return wl_container_of(server->outputs.next, output, link);
}
