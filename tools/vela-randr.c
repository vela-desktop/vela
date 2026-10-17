// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-randr: configures outputs as Settings > Display does
// (wlr-output-management), for tests without wlr-randr.
//
// Usage: vela-randr                          lists the outputs
//        vela-randr --output NAME [--on | --off] [--scale S] [--pos X,Y]
//                   [--mode WxH] [--transform normal|90|180|270|flipped|flipped-90|flipped-180|flipped-270]
//
// The other outputs stay as they are. Exits with 0 if Vela applies the
// configuration, 1 if it refuses it.

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "wlr-output-management-unstable-v1-client-protocol.h"

struct mode {
    struct zwlr_output_mode_v1* wl;
    int width, height, refresh;
    struct mode* next;
};

struct head {
    struct zwlr_output_head_v1* wl;
    char name[64];
    int enabled;
    int x, y;
    double scale;
    struct mode* current;
    struct head* next;
};

static struct zwlr_output_manager_v1* manager;
static struct head* heads;
static struct mode* modes;
static uint32_t serial;
static int done;
static int result = -1; // 1 applied, 0 refused

// ----------------------------------------------------------------- modes --

static void onModeSize(void* data, struct zwlr_output_mode_v1* wl, int32_t width, int32_t height)
{
    struct mode* mode = data;
    mode->width = width;
    mode->height = height;
}

static void onModeRefresh(void* data, struct zwlr_output_mode_v1* wl, int32_t refresh)
{
    ((struct mode*)data)->refresh = refresh;
}

static void onModePreferred(void* data, struct zwlr_output_mode_v1* wl) { }

static void onModeFinished(void* data, struct zwlr_output_mode_v1* wl) { }

static const struct zwlr_output_mode_v1_listener modeListener = {
    onModeSize,
    onModeRefresh,
    onModePreferred,
    onModeFinished,
};

// --------------------------------------------------------------- outputs --

static void onName(void* data, struct zwlr_output_head_v1* wl, const char* name)
{
    struct head* head = data;
    snprintf(head->name, sizeof(head->name), "%s", name);
}

static void onDescription(void* data, struct zwlr_output_head_v1* wl, const char* description) { }

static void onPhysicalSize(void* data, struct zwlr_output_head_v1* wl, int32_t width, int32_t height) { }

static void onMode(void* data, struct zwlr_output_head_v1* wl, struct zwlr_output_mode_v1* wlMode)
{
    struct mode* mode = calloc(1, sizeof(*mode));
    mode->wl = wlMode;
    mode->next = modes;
    modes = mode;
    zwlr_output_mode_v1_add_listener(wlMode, &modeListener, mode);
}

static void onEnabled(void* data, struct zwlr_output_head_v1* wl, int32_t enabled)
{
    ((struct head*)data)->enabled = enabled;
}

static void onCurrentMode(void* data, struct zwlr_output_head_v1* wl, struct zwlr_output_mode_v1* wlMode)
{
    struct head* head = data;
    for (struct mode* mode = modes; mode; mode = mode->next) {
        if (mode->wl == wlMode) {
            head->current = mode;
        }
    }
}

static void onPosition(void* data, struct zwlr_output_head_v1* wl, int32_t x, int32_t y)
{
    struct head* head = data;
    head->x = x;
    head->y = y;
}

static void onTransform(void* data, struct zwlr_output_head_v1* wl, int32_t transform) { }

static void onScale(void* data, struct zwlr_output_head_v1* wl, wl_fixed_t scale)
{
    ((struct head*)data)->scale = wl_fixed_to_double(scale);
}

static void onHeadFinished(void* data, struct zwlr_output_head_v1* wl) { }

static const struct zwlr_output_head_v1_listener headListener = {
    .name = onName,
    .description = onDescription,
    .physical_size = onPhysicalSize,
    .mode = onMode,
    .enabled = onEnabled,
    .current_mode = onCurrentMode,
    .position = onPosition,
    .transform = onTransform,
    .scale = onScale,
    .finished = onHeadFinished,
};

static void onHead(void* data, struct zwlr_output_manager_v1* wl, struct zwlr_output_head_v1* wlHead)
{
    struct head* head = calloc(1, sizeof(*head));
    head->wl = wlHead;
    head->scale = 1.0;
    head->next = heads;
    heads = head;
    zwlr_output_head_v1_add_listener(wlHead, &headListener, head);
}

static void onDone(void* data, struct zwlr_output_manager_v1* wl, uint32_t doneSerial)
{
    serial = doneSerial;
    done = 1;
}

static void onManagerFinished(void* data, struct zwlr_output_manager_v1* wl) { }

static const struct zwlr_output_manager_v1_listener managerListener = { onHead, onDone, onManagerFinished };

// ---------------------------------------------------------- configuration --

static void onSucceeded(void* data, struct zwlr_output_configuration_v1* config)
{
    result = 1;
}

static void onFailed(void* data, struct zwlr_output_configuration_v1* config)
{
    result = 0;
}

static void onCancelled(void* data, struct zwlr_output_configuration_v1* config)
{
    result = 0;
}

static const struct zwlr_output_configuration_v1_listener configListener = { onSucceeded, onFailed, onCancelled };

static void onGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (!strcmp(interface, zwlr_output_manager_v1_interface.name)) {
        manager = wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, 1);
        zwlr_output_manager_v1_add_listener(manager, &managerListener, NULL);
    }
}

static void onGlobalRemove(void* data, struct wl_registry* registry, uint32_t name) { }

static const struct wl_registry_listener registryListener = { onGlobal, onGlobalRemove };

static void usage(void)
{
    fprintf(stderr, "usage: vela-randr [--output NAME [--on | --off] [--scale S] [--pos X,Y] [--mode WxH] [--transform ROTATION]]\n");
    exit(2);
}

static int parseTransform(const char* name)
{
    static const char* const transforms[] = {
        "normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180", "flipped-270"
    };
    for (int i = 0; i < 8; ++i) {
        if (!strcmp(name, transforms[i])) {
            return i;
        }
    }
    return -1;
}

int main(int argc, char** argv)
{
    const char* target = NULL;
    int on = -1;
    double scale = 0.0;
    int hasPosition = 0, x = 0, y = 0;
    int width = 0, height = 0;
    int transform = -1;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(arg, "--on") || !strcmp(arg, "--off")) {
            on = !strcmp(arg, "--on");
        } else if (!strcmp(arg, "--output") && value) {
            target = value;
            ++i;
        } else if (!strcmp(arg, "--scale") && value) {
            scale = strtod(value, NULL);
            ++i;
        } else if (!strcmp(arg, "--pos") && value && sscanf(value, "%d,%d", &x, &y) == 2) {
            hasPosition = 1;
            ++i;
        } else if (!strcmp(arg, "--mode") && value && sscanf(value, "%dx%d", &width, &height) == 2) {
            ++i;
        } else if (!strcmp(arg, "--transform") && value && (transform = parseTransform(value)) >= 0) {
            ++i;
        } else {
            usage();
        }
    }

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "vela-randr: no Wayland display\n");
        return 1;
    }
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, NULL);
    wl_display_roundtrip(display);
    if (!manager) {
        fprintf(stderr, "vela-randr: the compositor has no wlr-output-management\n");
        return 1;
    }
    while (!done && wl_display_dispatch(display) != -1) { }

    if (!target) {
        for (struct head* head = heads; head; head = head->next) {
            if (!head->enabled) {
                printf("%s off\n", head->name);
                continue;
            }
            const struct mode* mode = head->current;
            printf("%s %dx%d@%.3f %d,%d %.4f\n", head->name, mode ? mode->width : 0, mode ? mode->height : 0,
                mode ? mode->refresh / 1000.0 : 0.0, head->x, head->y, head->scale);
        }
        return 0;
    }

    struct zwlr_output_configuration_v1* config = zwlr_output_manager_v1_create_configuration(manager, serial);
    zwlr_output_configuration_v1_add_listener(config, &configListener, NULL);
    int found = 0;
    for (struct head* head = heads; head; head = head->next) {
        const int mine = !strcmp(head->name, target);
        found |= mine;
        const int enabled = mine && on >= 0 ? on : head->enabled;
        if (!enabled) {
            zwlr_output_configuration_v1_disable_head(config, head->wl);
            continue;
        }
        struct zwlr_output_configuration_head_v1* wanted = zwlr_output_configuration_v1_enable_head(config, head->wl);
        if (!mine) {
            continue;
        }
        if (scale > 0.0) {
            zwlr_output_configuration_head_v1_set_scale(wanted, wl_fixed_from_double(scale));
        }
        if (hasPosition) {
            zwlr_output_configuration_head_v1_set_position(wanted, x, y);
        }
        if (width > 0 && height > 0) {
            zwlr_output_configuration_head_v1_set_custom_mode(wanted, width, height, 0);
        }
        if (transform >= 0) {
            zwlr_output_configuration_head_v1_set_transform(wanted, transform);
        }
    }
    if (!found) {
        fprintf(stderr, "vela-randr: no output named %s\n", target);
        return 1;
    }
    zwlr_output_configuration_v1_apply(config);
    while (result < 0 && wl_display_dispatch(display) != -1) { }
    if (result != 1) {
        fprintf(stderr, "vela-randr: the compositor refused the configuration\n");
    }
    return result == 1 ? 0 : 1;
}
