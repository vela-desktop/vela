// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "a11y.h"

#include "color.h"
#include "config.h"
#include "input.h"
#include "output.h"
#include "scene/frame.h"
#include "server.h"
#include "shell.h"
#include "sun.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "scene/scene.h"

static void announce(struct vela_a11y *a11y);

#define STICKY_MASK (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

static const char *yes_no(bool on)
{
    return on ? "yes" : "no";
}

// ------------------------------------------------------------------- sun --

// The system time zone's coordinates, from zone1970.tab (or zone.tab):
// "+4154+01229" for Europe/Rome. Good enough for sun times.
static bool timezone_coordinates(double *latitude, double *longitude)
{
    char zone[256];
    const char *tz = getenv("TZ");
    if (tz && *tz) {
        snprintf(zone, sizeof(zone), "%s", tz[0] == ':' ? tz + 1 : tz);
    } else {
        char target[512];
        ssize_t n = readlink("/etc/localtime", target, sizeof(target) - 1);
        if (n <= 0) {
            return false;
        }
        target[n] = '\0';
        const char *at = strstr(target, "zoneinfo/");
        if (!at) {
            return false;
        }
        snprintf(zone, sizeof(zone), "%s", at + 9);
    }
    const char *tables[] = { "/usr/share/zoneinfo/zone1970.tab", "/usr/share/zoneinfo/zone.tab" };
    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); ++t) {
        FILE *in = fopen(tables[t], "re");
        if (!in) {
            continue;
        }
        char line[512];
        while (fgets(line, sizeof(line), in)) {
            char codes[128], coordinates[64], name[256];
            if (line[0] == '#' || sscanf(line, "%127s %63s %255s", codes, coordinates, name) != 3
                || strcmp(name, zone) != 0) {
                continue;
            }
            fclose(in);
            return vela_sun_parse_coordinates(coordinates, latitude, longitude);
        }
        fclose(in);
    }
    return false;
}

// ----------------------------------------------------------------- color --

/**
 * Publish color-filter and Night Light state to the scene and each output.
 * Switching backends invalidates rendering even if the gains remain constant
 * and explicitly retries any previously rejected hardware LUT clear.
 */
static void apply_color(struct vela_a11y *a11y)
{
    struct vela_scene *scene = a11y->server->scene;
    float m[9];
    memcpy(m, vela_identity, sizeof(m));
    if (a11y->color_filter) {
        vela_filter_matrix(a11y->color_filter_kind, m);
    }
    float gains[3] = { 1.0f, 1.0f, 1.0f };
    bool night = a11y->night_level > 0.0;
    if (night) {
        vela_night_gains(6500.0 + (vela_night_kelvin(a11y->night_strength) - 6500.0) * a11y->night_level, gains);
    }
    bool filter_changed = a11y->color_filter != scene->color_filtered
        || (a11y->color_filter && memcmp(m, scene->color_filter, sizeof(m)) != 0);
    bool night_changed = night != scene->night_active || memcmp(gains, scene->night_gains, sizeof(gains)) != 0;
    bool backend_changed = a11y->night_nvidia_fix != scene->night_force_shader;
    scene->night_force_shader = a11y->night_nvidia_fix;
    scene->color_filtered = a11y->color_filter;
    memcpy(scene->color_filter, m, sizeof(m));
    scene->night_active = night;
    memcpy(scene->night_gains, gains, sizeof(gains));
    if (night_changed || backend_changed) {
        ++scene->night_version;
    }
    if (filter_changed || night_changed || backend_changed) {
        struct vela_output *output;
        wl_list_for_each (output, &a11y->server->outputs, link) {
            // A manual switch in either direction must retry a previously
            // rejected LUT clear. Otherwise enabling the workaround can leave
            // the broken hardware LUT installed until the periodic retry.
            if (backend_changed) {
                vela_output_frame_retry_night_light(output->frame);
            }
            vela_output_frame_color_changed(output->frame, filter_changed, night_changed || backend_changed);
        }
    }
}

// Today's sunset and sunrise here, in minutes after midnight.
static bool sun_today(int *set, int *rise)
{
    double latitude = 0.0;
    double longitude = 0.0;
    if (!timezone_coordinates(&latitude, &longitude)) {
        return false;
    }
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    *set = vela_sun_time(false, &local, latitude, longitude);
    *rise = vela_sun_time(true, &local, latitude, longitude);
    return *set >= 0 && *rise >= 0;
}

static void check_night_schedule(struct vela_a11y *a11y)
{
    bool sunset = strcmp(a11y->schedule, "sunset") == 0;
    if (!sunset && strcmp(a11y->schedule, "hours") != 0) {
        a11y->scheduled = -1;
        return;
    }
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    int minutes = local.tm_hour * 60 + local.tm_min;
    int from = a11y->night_from;
    int to = a11y->night_to;
    int set = 0;
    int rise = 0;
    if (sunset && sun_today(&set, &rise)) {
        from = set;
        to = rise;
    }
    int wanted = vela_in_range(minutes, from, to) ? 1 : 0;
    if (wanted == a11y->scheduled) {
        return; // no change: the choice made by hand stays
    }
    a11y->scheduled = wanted;
    if ((bool)wanted != a11y->night_light) {
        wlr_log(WLR_INFO, "Night light: %s by the schedule (%02d:%02d-%02d:%02d)", wanted ? "on" : "off", from / 60,
            from % 60, to / 60, to % 60);
        vela_a11y_set_night_light(a11y, wanted, true);
    }
}

// A check per minute is enough (and after a suspend it catches up at once).
static int handle_schedule_timer(void *data)
{
    struct vela_a11y *a11y = data;
    check_night_schedule(a11y);
    // A failed gamma test/commit can be temporary. A pending LUT clear must
    // also retry when Night Light is OFF: the monitor may still be tinted.
    struct vela_output *output;
    wl_list_for_each (output, &a11y->server->outputs, link) {
        if (a11y->night_light || output->frame->night_clear_needed) {
            vela_output_frame_retry_night_light(output->frame);
        }
    }
    wl_event_source_timer_update(a11y->timer, 60000);
    return 0;
}

// -------------------------------------------------------------- creation --

struct vela_a11y *vela_a11y_create(struct vela_server *server)
{
    struct vela_a11y *a11y = calloc(1, sizeof(*a11y));
    a11y->server = server;
    a11y->night_strength = 48;
    snprintf(a11y->schedule, sizeof(a11y->schedule), "no");
    a11y->night_from = 21 * 60;
    a11y->night_to = 7 * 60;
    a11y->scheduled = -1;
    a11y->level_tween = (struct vela_tween) { -1.0, 1.0, NULL };
    snprintf(a11y->color_filter_kind, sizeof(a11y->color_filter_kind), "grayscale");
    a11y->zoom_step = 100;
    a11y->zoom_target = 1.0;
    a11y->zoom = 1.0;
    a11y->zoom_from = 1.0;
    a11y->zoom_tween = (struct vela_tween) { -1.0, 1.0, NULL };
    a11y->timer = wl_event_loop_add_timer(server->loop, handle_schedule_timer, a11y);
    wl_event_source_timer_update(a11y->timer, 60000);
    return a11y;
}

void vela_a11y_destroy(struct vela_a11y *a11y)
{
    if (!a11y) {
        return;
    }
    wl_event_source_remove(a11y->timer);
    free(a11y);
}

// Like Windows: the screen warms up (or cools down) over one second.
static void start_night_transition(struct vela_a11y *a11y)
{
    a11y->level_from = a11y->night_level;
    vela_tween_start(&a11y->level_tween, "night-light", 1000.0, &vela_decelerate);
    a11y->level_animating = true;
    vela_server_schedule_frames(a11y->server);
}

/** Reload accessibility preferences, including the optional Night Light backend. */
void vela_a11y_load(struct vela_a11y *a11y)
{
    struct vela_config settings;
    vela_config_read(&settings);
    a11y->night_strength = vela_clamp(atoi(vela_config_get(&settings, "night-light-strength", "48")), 0, 100);
    a11y->night_nvidia_fix = vela_config_flag(&settings, "night-light-nvidia-fix", false);
    snprintf(a11y->schedule, sizeof(a11y->schedule), "%s", vela_config_get(&settings, "night-light-schedule", "no"));
    a11y->night_from = vela_parse_clock(vela_config_get(&settings, "night-light-from", ""), 21 * 60);
    a11y->night_to = vela_parse_clock(vela_config_get(&settings, "night-light-to", ""), 7 * 60);
    snprintf(a11y->color_filter_kind, sizeof(a11y->color_filter_kind), "%s",
        vela_config_get(&settings, "color-filter", "grayscale"));
    a11y->color_filter_shortcut = vela_config_flag(&settings, "color-filters-shortcut", false);
    a11y->zoom_step = vela_clamp(atoi(vela_config_get(&settings, "magnifier-step", "100")), 25, 400);
    bool night = vela_config_flag(&settings, "night-light", false);
    bool filter = vela_config_flag(&settings, "color-filters", false);
    bool sticky = vela_config_flag(&settings, "sticky-keys", false);
    vela_motion_set_effects(vela_config_flag(&settings, "animation-effects", true));
    int duration = vela_clamp(atoi(vela_config_get(&settings, "animation-duration", "100")), 50, 300);
    vela_motion_set_duration(duration / 100.0);
    vela_config_finish(&settings);

    // A new (or changed) schedule decides at once; otherwise the file's value
    // holds (even if the user chose differently from the schedule by hand,
    // until its next change).
    char key[64];
    snprintf(key, sizeof(key), "%s %d %d", a11y->schedule, a11y->night_from, a11y->night_to);
    if (strcmp(key, a11y->schedule_key) != 0) {
        a11y->scheduled = -1;
    }
    snprintf(a11y->schedule_key, sizeof(a11y->schedule_key), "%s", key);
    a11y->night_light = night;
    a11y->color_filter = filter;
    a11y->sticky_keys = sticky;
    if (!sticky) {
        a11y->latched = 0;
        a11y->locked = 0;
    }
    check_night_schedule(a11y);
    // No gradual change at startup: as it was.
    if (a11y->night_level == 0.0 && a11y->night_light && !a11y->level_animating && !a11y->loaded) {
        a11y->night_level = 1.0;
    }
    a11y->loaded = true;
    if (a11y->night_light != (a11y->night_level > 0.5) && !a11y->level_animating) {
        start_night_transition(a11y);
    }
    apply_color(a11y);
    announce(a11y);
}

// -------------------------------------------------------------- choices --

void vela_a11y_set_night_light(struct vela_a11y *a11y, bool on, bool save)
{
    if (save) {
        vela_config_write("night-light", yes_no(on));
    }
    bool activating = on && !a11y->night_light;
    // Retrying a failed LUT clear must work even when Night Light is already
    // off and its fade has finished. Do this before the unchanged-state return,
    // without starting another animation or retrying on every animation tick.
    if (activating || !on) {
        struct vela_output *output;
        wl_list_for_each (output, &a11y->server->outputs, link) {
            if (activating || output->frame->night_clear_needed) {
                vela_output_frame_retry_night_light(output->frame);
            }
        }
    }
    if (on == a11y->night_light && !a11y->level_animating && a11y->night_level == (on ? 1.0 : 0.0)) {
        return;
    }
    a11y->night_light = on;
    start_night_transition(a11y);
    announce(a11y);
}

void vela_a11y_set_color_filter(struct vela_a11y *a11y, bool on, bool save)
{
    if (save) {
        vela_config_write("color-filters", yes_no(on));
    }
    if (on == a11y->color_filter) {
        return;
    }
    a11y->color_filter = on;
    apply_color(a11y);
    announce(a11y);
}

void vela_a11y_set_sticky_keys(struct vela_a11y *a11y, bool on, bool save)
{
    if (save) {
        vela_config_write("sticky-keys", yes_no(on));
    }
    if (on == a11y->sticky_keys) {
        return;
    }
    a11y->sticky_keys = on;
    if (!on && (a11y->latched || a11y->locked)) {
        a11y->latched = 0;
        a11y->locked = 0;
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(a11y->server->seat);
        if (keyboard) {
            wlr_keyboard_notify_modifiers(keyboard, keyboard->modifiers.depressed, 0,
                keyboard->modifiers.locked & ~STICKY_MASK, keyboard->modifiers.group);
        }
    }
    a11y->candidate = 0;
    announce(a11y);
}

// ------------------------------------------------------------- magnifier --

// Zooming is movement: without animation effects it happens at once.
static double zoom_ms(void)
{
    return vela_motion_effects() ? vela_motion_ms(VELA_MAGNIFIER_MS) : 0.0;
}

void vela_a11y_set_magnifier(struct vela_a11y *a11y, bool on)
{
    if (on == a11y->magnifier) {
        return;
    }
    a11y->magnifier = on;
    // Like Windows: it starts at double.
    a11y->zoom_target = on ? 1.0 + a11y->zoom_step / 100.0 : 1.0;
    a11y->zoom_from = a11y->zoom;
    vela_tween_start(&a11y->zoom_tween, "magnifier", zoom_ms(), &vela_decelerate);
    a11y->zoom_animating = true;
    struct wlr_cursor *cursor = a11y->server->cursor;
    if (on && !a11y->zoom_output) {
        a11y->zoom_output = vela_output_at(a11y->server, cursor->x, cursor->y);
        a11y->view_x = a11y->zoom_output ? vela_output_box(a11y->zoom_output).x : 0.0;
        a11y->view_y = a11y->zoom_output ? vela_output_box(a11y->zoom_output).y : 0.0;
        a11y->anchor_x = (cursor->x - a11y->view_x) * a11y->zoom;
        a11y->anchor_y = (cursor->y - a11y->view_y) * a11y->zoom;
    }
    wlr_log(WLR_INFO, "Magnifier %s", on ? "open" : "closed");
    vela_server_schedule_frames(a11y->server);
    announce(a11y);
}

void vela_a11y_zoom(struct vela_a11y *a11y, int direction)
{
    if (!a11y->magnifier) {
        if (direction > 0) {
            vela_a11y_set_magnifier(a11y, true);
        }
        return;
    }
    double step = a11y->zoom_step / 100.0;
    double target = vela_clampd(a11y->zoom_target + direction * step, 1.0, 16.0);
    if (target == a11y->zoom_target) {
        return;
    }
    a11y->zoom_target = target;
    a11y->zoom_from = a11y->zoom;
    vela_tween_start(&a11y->zoom_tween, "magnifier", zoom_ms(), &vela_decelerate);
    a11y->zoom_animating = true;
    vela_server_schedule_frames(a11y->server);
}

// The cursors drawn on an output, in its logical buffer coordinates.
static void move_output_cursors(struct vela_output *output, double x, double y)
{
    struct wlr_output_cursor *c;
    wl_list_for_each (c, &output->wlr->cursors, link) {
        wlr_output_cursor_move(c, x, y);
    }
}

// Shows the zoomed view at the current zoom, around view_x/view_y.
static void update_magnifier(struct vela_a11y *a11y)
{
    struct vela_server *server = a11y->server;
    struct wlr_cursor *cursor = server->cursor;
    struct vela_output *out = vela_output_at(server, cursor->x, cursor->y);
    bool zoomed = a11y->zoom > 1.0;
    struct vela_output *output;
    wl_list_for_each (output, &server->outputs, link) {
        if ((output != out || !zoomed) && output->frame->zoom > 1.0) {
            vela_output_frame_set_magnifier(output->frame, 1.0, 0.0, 0.0);
            // The cursor goes back where wlr_cursor puts it (the output's
            // logical point).
            struct wlr_box box = vela_output_box(output);
            move_output_cursors(output, cursor->x - box.x, cursor->y - box.y);
        }
    }
    if (!zoomed || !out) {
        a11y->zoom_output = zoomed ? NULL : a11y->zoom_output;
        return;
    }
    struct wlr_box box = vela_output_box(out);
    if (out != a11y->zoom_output) {
        // Another output: the zoomed area starts around the cursor.
        a11y->zoom_output = out;
        a11y->view_x = cursor->x - (cursor->x - box.x) / a11y->zoom;
        a11y->view_y = cursor->y - (cursor->y - box.y) / a11y->zoom;
        a11y->anchor_x = cursor->x - box.x;
        a11y->anchor_y = cursor->y - box.y;
    }
    double w = box.width / a11y->zoom;
    double h = box.height / a11y->zoom;
    // The cursor pushes the area when it reaches its edges.
    a11y->view_x = vela_clampd(a11y->view_x, cursor->x - w, cursor->x);
    a11y->view_y = vela_clampd(a11y->view_y, cursor->y - h, cursor->y);
    a11y->view_x = vela_clampd(a11y->view_x, box.x, box.x + box.width - w);
    a11y->view_y = vela_clampd(a11y->view_y, box.y, box.y + box.height - h);
    vela_output_frame_set_magnifier(out->frame, a11y->zoom, a11y->view_x, a11y->view_y);
    // The cursor where the point it indicates is seen.
    move_output_cursors(out, (cursor->x - a11y->view_x) * a11y->zoom, (cursor->y - a11y->view_y) * a11y->zoom);
}

void vela_a11y_update_magnifier(struct vela_a11y *a11y)
{
    update_magnifier(a11y);
    if (a11y->zoom_output) {
        struct wlr_cursor *cursor = a11y->server->cursor;
        a11y->anchor_x = (cursor->x - a11y->view_x) * a11y->zoom;
        a11y->anchor_y = (cursor->y - a11y->view_y) * a11y->zoom;
    }
}

bool vela_a11y_tick(struct vela_a11y *a11y, double now_ms, double wall_ms)
{
    bool running = false;
    if (a11y->level_animating) {
        double p = vela_tween_progress(&a11y->level_tween, now_ms);
        double target = a11y->night_light ? 1.0 : 0.0;
        a11y->night_level = a11y->level_from + (target - a11y->level_from) * p;
        if (vela_tween_retired(&a11y->level_tween, wall_ms)) {
            a11y->night_level = target;
            a11y->level_animating = false;
        }
        apply_color(a11y);
        running = running || a11y->level_animating;
    }
    if (a11y->zoom_animating) {
        double p = vela_tween_progress(&a11y->zoom_tween, now_ms);
        a11y->zoom = a11y->zoom_from + (a11y->zoom_target - a11y->zoom_from) * p;
        if (vela_tween_retired(&a11y->zoom_tween, wall_ms)) {
            a11y->zoom = a11y->zoom_target;
            a11y->zoom_animating = false;
        }
        // The point under the cursor stays still on screen.
        struct wlr_cursor *cursor = a11y->server->cursor;
        if (a11y->zoom_output && a11y->zoom > 0.0) {
            a11y->view_x = cursor->x - a11y->anchor_x / a11y->zoom;
            a11y->view_y = cursor->y - a11y->anchor_y / a11y->zoom;
        }
        if (!a11y->zoom_animating && a11y->zoom <= 1.0 && !a11y->magnifier) {
            a11y->zoom = 1.0;
            // Closed: the next opening anchors at the pointer, not at the
            // point this one kept still.
            a11y->zoom_output = NULL;
        }
        update_magnifier(a11y);
        running = running || a11y->zoom_animating;
    }
    return running;
}

void vela_a11y_output_destroyed(struct vela_a11y *a11y, struct vela_output *output)
{
    if (a11y->zoom_output == output) {
        a11y->zoom_output = NULL;
    }
}

// ----------------------------------------------------------- sticky keys --

static uint32_t sticky_bit(xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
        return WLR_MODIFIER_SHIFT;
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
        return WLR_MODIFIER_CTRL;
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
        return WLR_MODIFIER_ALT;
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
        return WLR_MODIFIER_LOGO;
    default:
        return 0;
    }
}

static void apply_sticky(struct vela_a11y *a11y, struct wlr_keyboard *k)
{
    wlr_keyboard_notify_modifiers(k, k->modifiers.depressed, a11y->latched,
        (k->modifiers.locked & ~STICKY_MASK) | a11y->locked, k->modifiers.group);
}

bool vela_a11y_sticky_key(struct vela_a11y *a11y, struct wlr_keyboard *keyboard, const xkb_keysym_t *syms,
    int count, bool pressed)
{
    if (!a11y->sticky_keys) {
        return false;
    }
    uint32_t bit = 0;
    for (int i = 0; i < count; ++i) {
        bit |= sticky_bit(syms[i]);
    }
    if (!bit) {
        a11y->candidate = 0;
        // The next key used them: they are let go when it's released.
        if (!pressed && a11y->latched) {
            a11y->latched = 0;
            apply_sticky(a11y, keyboard);
        }
        return false;
    }
    if (pressed) {
        a11y->candidate = bit;
        return false;
    }
    if (a11y->candidate != bit) {
        return false; // used with another key: nothing to remember
    }
    a11y->candidate = 0;
    bool super_latched = false;
    if (bit == WLR_MODIFIER_LOGO) {
        // Win: the first time it stays pressed for the next key; again, it
        // opens Start (like the key alone).
        if (a11y->latched & bit) {
            a11y->latched &= ~bit;
        } else {
            a11y->latched |= bit;
            super_latched = true;
        }
    } else if (a11y->locked & bit) {
        a11y->locked &= ~bit;
    } else if (a11y->latched & bit) {
        a11y->latched &= ~bit;
        a11y->locked |= bit;
    } else {
        a11y->latched |= bit;
    }
    apply_sticky(a11y, keyboard);
    return super_latched;
}

// ------------------------------------------------------------ the shell --

/** Serialize accessibility flags, the NVIDIA fallback setting, and sun times. */
bool vela_a11y_json(const struct vela_a11y *a11y, char *out, size_t size)
{
    // Today's sun times, for the night light page ("Sunset to sunrise").
    char sun[64] = "";
    int set = 0;
    int rise = 0;
    if (sun_today(&set, &rise)) {
        snprintf(sun, sizeof(sun), ",\"sunset\":\"%02d:%02d\",\"sunrise\":\"%02d:%02d\"", set / 60, set % 60,
            rise / 60, rise % 60);
    }
    const char *t = "true";
    const char *f = "false";
    return vela_format(out, size,
        "{\"nightLight\":%s,\"nightLightNvidiaFix\":%s,\"colorFilter\":%s,\"magnifier\":%s,\"stickyKeys\":%s,\"touchpad\":%s%s}",
        a11y->night_light ? t : f, a11y->night_nvidia_fix ? t : f, a11y->color_filter ? t : f,
        a11y->magnifier ? t : f, a11y->sticky_keys ? t : f,
        vela_input_has_touchpad(a11y->server->input) ? t : f, sun);
}

static void announce(struct vela_a11y *a11y)
{
    char line[512] = "accessibility ";
    size_t used = strlen(line);
    if (vela_a11y_json(a11y, line + used, sizeof(line) - used)) {
        vela_shell_send(a11y->server, line);
    }
}
