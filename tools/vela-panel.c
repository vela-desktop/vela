// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-panel: fake shell pieces for tests (wlr-layer-shell), so tests
// without the Qt shell cover layers, reserved space and blur
// (ext-background-effect).
//
// Usage: vela-panel wallpaper [--color RRGGBB]
//                                     fullscreen wallpaper, a striped
//                                     gradient (something to blur), or one
//                                     flat color
//        vela-panel taskbar [--blur] [--clear] [--namespace NAME]
//        vela-panel menu          TOP/ON_DEMAND layer surface simulating Start
//                                     48-high translucent bottom bar,
//                                     reserving 48 units; --blur asks to
//                                     blur what lies behind; --clear: black
//                                     at 50% instead of a dark gray (what's
//                                     below shows unchanged, halved); the
//                                     layer namespace, vela-panel by default
//
// Draws at scale 1 in shared memory and stays until closed.

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-background-effect-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct zwlr_layer_shell_v1* layerShell;
static struct ext_background_effect_manager_v1* effects;

static struct wl_surface* surface;
static struct zwlr_layer_surface_v1* layerSurface;
static struct ext_background_effect_surface_v1* effect; // with --blur
static int wallpaper;
static int flat = -1; // the wallpaper's one color (--color), or -1
static int clear;
static int running = 1;

static void onGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (!strcmp(interface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        layerShell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 4);
    } else if (!strcmp(interface, ext_background_effect_manager_v1_interface.name)) {
        effects = wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1);
    }
}

static void onGlobalRemove(void* data, struct wl_registry* registry, uint32_t name) { }

static const struct wl_registry_listener registryListener = { onGlobal, onGlobalRemove };

static void onBufferRelease(void* data, struct wl_buffer* buffer)
{
    wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener bufferListener = { onBufferRelease };

static void draw(uint32_t width, uint32_t height)
{
    const int stride = (int)width * 4;
    const size_t size = (size_t)stride * height;
    const int fd = memfd_create("vela-panel", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        perror("vela-panel: memfd");
        exit(1);
    }
    uint32_t* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t color;
            if (wallpaper && flat >= 0) {
                color = 0xff000000u | (uint32_t)flat;
            } else if (wallpaper) {
                // Colored bands changing along x and y: the blur shows.
                const uint32_t r = (x * 255 / width) & 0xff;
                const uint32_t g = ((y / 24) % 2) ? 0xc0 : 0x30;
                const uint32_t b = (255 - y * 255 / height) & 0xff;
                color = 0xff000000u | r << 16 | g << 8 | b;
            } else {
                // Dark gray (or black) at 50%, premultiplied.
                color = clear ? 0x80000000u : 0x80101018u;
            }
            pixels[(size_t)y * width + x] = color;
        }
    }
    munmap(pixels, size);
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width, (int32_t)height, stride,
        wallpaper ? WL_SHM_FORMAT_XRGB8888 : WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(buffer, &bufferListener, NULL);
    if (effect) {
        // "Everything", the way toolkits ask for the whole surface.
        struct wl_region* region = wl_compositor_create_region(compositor);
        wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
        ext_background_effect_surface_v1_set_blur_region(effect, region);
        wl_region_destroy(region);
    }
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, (int32_t)width, (int32_t)height);
    wl_surface_commit(surface);
}

static void onConfigure(void* data, struct zwlr_layer_surface_v1* layer, uint32_t serial, uint32_t width,
    uint32_t height)
{
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (width > 0 && height > 0) {
        draw(width, height);
    }
}

static void onClosed(void* data, struct zwlr_layer_surface_v1* layer)
{
    running = 0;
}

static const struct zwlr_layer_surface_v1_listener layerListener = { onConfigure, onClosed };

int main(int argc, char** argv)
{
    if (argc < 2 || (strcmp(argv[1], "wallpaper") && strcmp(argv[1], "taskbar") && strcmp(argv[1], "menu"))) {
        fprintf(stderr, "usage: vela-panel wallpaper [--color RRGGBB] | taskbar [--blur] [--clear] "
                        "[--namespace NAME] | menu\n");
        return 2;
    }
    wallpaper = !strcmp(argv[1], "wallpaper");
    const int menu = !strcmp(argv[1], "menu");
    int blur = 0;
    const char* name = "vela-panel";
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--blur")) {
            blur = 1;
        } else if (!strcmp(argv[i], "--clear")) {
            clear = 1;
        } else if (!strcmp(argv[i], "--namespace") && i + 1 < argc) {
            name = argv[++i];
        } else if (!strcmp(argv[i], "--color") && i + 1 < argc) {
            flat = (int)(strtoul(argv[++i], NULL, 16) & 0xffffff);
        } else {
            fprintf(stderr, "vela-panel: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "vela-panel: no Wayland session (WAYLAND_DISPLAY)\n");
        return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registryListener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !layerShell || (blur && !effects)) {
        fprintf(stderr, "vela-panel: missing protocols (wlr-layer-shell, ext-background-effect)\n");
        return 1;
    }

    surface = wl_compositor_create_surface(compositor);
    layerSurface = zwlr_layer_shell_v1_get_layer_surface(layerShell, surface, NULL,
        wallpaper ? ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND : ZWLR_LAYER_SHELL_V1_LAYER_TOP, name);
    zwlr_layer_surface_v1_add_listener(layerSurface, &layerListener, NULL);
    if (wallpaper) {
        zwlr_layer_surface_v1_set_anchor(layerSurface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(layerSurface, -1);
    } else if (menu) {
        zwlr_layer_surface_v1_set_anchor(layerSurface, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
        zwlr_layer_surface_v1_set_size(layerSurface, 360, 280);
        zwlr_layer_surface_v1_set_exclusive_zone(layerSurface, 0);
        zwlr_layer_surface_v1_set_keyboard_interactivity(layerSurface,
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    } else {
        zwlr_layer_surface_v1_set_anchor(layerSurface, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(layerSurface, 0, 48);
        zwlr_layer_surface_v1_set_exclusive_zone(layerSurface, 48);
    }
    if (blur) {
        effect = ext_background_effect_manager_v1_get_background_effect(effects, surface);
    }
    wl_surface_commit(surface);

    while (running && wl_display_dispatch(display) != -1) {
    }
    return 0;
}
