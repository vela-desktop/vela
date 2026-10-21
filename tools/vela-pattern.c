// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-pattern: sharpness test client (docs/renderer.md §3.10).
//
// Opens an undecorated window and fills it, at the exact fractional scale
// the compositor asks for (fractional-scale-v1 + viewporter), with a buffer
// as large as its physical pixels. Each pixel encodes its own coordinates in
// the buffer:
//
//     R = x % 256,  G = y % 256,  B = (x / 256) * 16 + (y / 256)
//
// On screen, if the compositor copies the buffer 1:1, every pixel of the
// window "tells" the same origin: the test (scripts/test-sharpness.sh)
// finds it and compares everything bit for bit. Alternating rows and columns
// and close values are the worst case for any filter.
//
// Usage: vela-pattern [--scale1] [--app-id ID] [--decorated] [--popup X,Y]
//                     [--pointer-log FILE] [--inhibit-shortcuts]
//                     [WIDTH HEIGHT]   (logical; default 401x301)
//
// --scale1: like an old app, always draws at scale 1 (the compositor must
// magnify): one-pixel lines and a checkerboard, to judge the filter.
// --decorated: asks for Vela's title bar (xdg-decoration), like Qt and KDE
// apps.
// --popup X,Y: also opens a 200x150 magenta menu (xdg-popup), anchored at
// point (X, Y) of the window; it may slide to stay on screen.

#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fractional-scale-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wmBase;
static struct wp_viewporter* viewporter;
static struct wp_fractional_scale_manager_v1* fractionalManager;
static struct zxdg_decoration_manager_v1* decorationManager;

static struct wl_surface* surface;
static struct wp_viewport* viewport;
static struct xdg_surface* xdgSurface;
static struct xdg_toplevel* toplevel;

static int logicalWidth = 401;
static int logicalHeight = 301;
static int pendingWidth;
static int pendingHeight;
static uint32_t scale120 = 120;
static int configured;
static int legacy;
static int running = 1;

#define POPUP_WIDTH 200
#define POPUP_HEIGHT 150
static int popupWanted;
static int popupX;
static int popupY;
static struct wl_surface* popupSurface;
static struct wl_seat* seat;
static FILE* pointerLog;
static int inhibitShortcuts;
static struct zwp_relative_pointer_manager_v1* relativePointerManager;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1* shortcutsInhibitManager;

static void onGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (!strcmp(interface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
        decorationManager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        wmBase = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name) && (pointerLog || inhibitShortcuts) && !seat) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 9 ? version : 9);
    } else if (!strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) && pointerLog) {
        relativePointerManager = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (!strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) && inhibitShortcuts) {
        shortcutsInhibitManager
            = wl_registry_bind(registry, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    } else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
        fractionalManager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    }
}

static void onGlobalRemove(void* data, struct wl_registry* registry, uint32_t name) { }

static const struct wl_registry_listener registryListener = { onGlobal, onGlobalRemove };

static void onBufferRelease(void* data, struct wl_buffer* buffer)
{
    wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener bufferListener = { onBufferRelease };

// Buffer size as fractional-scale-v1 wants it: the logical size times the
// scale, rounded.
static int physical(int logical)
{
    return (int)((logical * (int64_t)scale120 + 60) / 120);
}

static void draw(void)
{
    const int width = physical(logicalWidth);
    const int height = physical(logicalHeight);
    const int stride = width * 4;
    const size_t size = (size_t)stride * height;

    const int fd = memfd_create("vela-pattern", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        perror("vela-pattern: memfd");
        exit(1);
    }
    uint32_t* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t color;
            if (legacy) {
                // Four quadrants: one-pixel horizontal and vertical lines, a
                // checkerboard, and a black border on white.
                const int left = x < width / 2;
                const int top = y < height / 2;
                const int on = top ? (left ? y & 1 : x & 1) : (left ? (x ^ y) & 1 : (x % 16 == 0 || y % 16 == 0));
                color = on ? 0xff000000u : 0xffffffffu;
            } else {
                const uint32_t r = (uint32_t)x & 255;
                const uint32_t g = (uint32_t)y & 255;
                const uint32_t b = ((uint32_t)(x >> 8) & 15) << 4 | ((uint32_t)(y >> 8) & 15);
                color = 0xff000000u | r << 16 | g << 8 | b;
            }
            pixels[y * width + x] = color;
        }
    }
    munmap(pixels, size);

    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(buffer, &bufferListener, NULL);
    wl_shm_pool_destroy(pool);
    close(fd);

    wp_viewport_set_destination(viewport, logicalWidth, logicalHeight);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    wl_surface_commit(surface);
    fprintf(stderr, "vela-pattern: %dx%d logical, scale %u/120, buffer %dx%d\n", logicalWidth, logicalHeight,
        scale120, width, height);
}

// The menu: a magenta rectangle at scale 1.
static void drawPopup(void)
{
    const int stride = POPUP_WIDTH * 4;
    const size_t size = (size_t)stride * POPUP_HEIGHT;
    const int fd = memfd_create("vela-pattern-popup", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        perror("vela-pattern: memfd");
        exit(1);
    }
    uint32_t* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < POPUP_WIDTH * POPUP_HEIGHT; ++i) {
        pixels[i] = 0xffff00ffu;
    }
    munmap(pixels, size);
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer* buffer
        = wl_shm_pool_create_buffer(pool, 0, POPUP_WIDTH, POPUP_HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(buffer, &bufferListener, NULL);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(popupSurface, buffer, 0, 0);
    wl_surface_damage_buffer(popupSurface, 0, 0, POPUP_WIDTH, POPUP_HEIGHT);
    wl_surface_commit(popupSurface);
}

static void onPopupSurfaceConfigure(void* data, struct xdg_surface* xdg, uint32_t serial)
{
    xdg_surface_ack_configure(xdg, serial);
    drawPopup();
}

static const struct xdg_surface_listener popupSurfaceListener = { onPopupSurfaceConfigure };

static void onPopupConfigure(void* data, struct xdg_popup* popup, int32_t x, int32_t y, int32_t width, int32_t height)
{
    fprintf(stderr, "vela-pattern: popup at %d,%d (%dx%d)\n", x, y, width, height);
}

static void onPopupDone(void* data, struct xdg_popup* popup) { }
static void onPopupRepositioned(void* data, struct xdg_popup* popup, uint32_t token) { }

static const struct xdg_popup_listener popupListener = { onPopupConfigure, onPopupDone, onPopupRepositioned };

static void openPopup(void)
{
    struct xdg_positioner* positioner = xdg_wm_base_create_positioner(wmBase);
    xdg_positioner_set_size(positioner, POPUP_WIDTH, POPUP_HEIGHT);
    xdg_positioner_set_anchor_rect(positioner, popupX, popupY, 1, 1);
    xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(positioner,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    popupSurface = wl_compositor_create_surface(compositor);
    struct xdg_surface* xdg = xdg_wm_base_get_xdg_surface(wmBase, popupSurface);
    xdg_surface_add_listener(xdg, &popupSurfaceListener, NULL);
    struct xdg_popup* popup = xdg_surface_get_popup(xdg, xdgSurface, positioner);
    xdg_popup_add_listener(popup, &popupListener, NULL);
    xdg_positioner_destroy(positioner);
    wl_surface_commit(popupSurface);
}

// ------------------------------------------------------------ pointer log --

static void logLine(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void logLine(const char* format, ...)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(pointerLog, "%lld ", (long long)now.tv_sec * 1000000 + now.tv_nsec / 1000);
    va_list args;
    va_start(args, format);
    vfprintf(pointerLog, format, args);
    va_end(args);
    fputc('\n', pointerLog);
    fflush(pointerLog);
}

static void onEnter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y)
{
    logLine("enter %.3f %.3f", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void onLeave(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s) { logLine("leave"); }
static void onMotion(void* d, struct wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    logLine("motion %.3f %.3f", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void onButton(void* d, struct wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    logLine("button %u %u", button, state);
}
static void onAxis(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    logLine("axis %u %.6f", axis, wl_fixed_to_double(value));
}
static void onFrame(void* d, struct wl_pointer* p) { logLine("frame"); }
static void onAxisSource(void* d, struct wl_pointer* p, uint32_t source) { logLine("source %u", source); }
static void onAxisStop(void* d, struct wl_pointer* p, uint32_t time, uint32_t axis) { logLine("stop %u", axis); }
static void onAxisDiscrete(void* d, struct wl_pointer* p, uint32_t axis, int32_t n)
{
    logLine("discrete %u %d", axis, n);
}
static void onAxisValue120(void* d, struct wl_pointer* p, uint32_t axis, int32_t n)
{
    logLine("value120 %u %d", axis, n);
}
static void onAxisDirection(void* d, struct wl_pointer* p, uint32_t axis, uint32_t direction) { }

static const struct wl_pointer_listener pointerListener = {
    .enter = onEnter, .leave = onLeave, .motion = onMotion, .button = onButton, .axis = onAxis, .frame = onFrame,
    .axis_source = onAxisSource, .axis_stop = onAxisStop, .axis_discrete = onAxisDiscrete,
    .axis_value120 = onAxisValue120, .axis_relative_direction = onAxisDirection,
};

// Logging the less common pointer/keyboard input paths makes it possible to
// catch compositor bugs where a swallowed press still leaks its release.
static void onRelativeMotion(void* data, struct zwp_relative_pointer_v1* relative, uint32_t hi, uint32_t lo,
    wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t unaccelDx, wl_fixed_t unaccelDy)
{
    logLine("relative %.3f %.3f", wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}
static const struct zwp_relative_pointer_v1_listener relativePointerListener = {
    .relative_motion = onRelativeMotion,
};

static void onKeyboardKeymap(void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    close(fd);
}
static void onKeyboardEnter(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface,
    struct wl_array* keys) { }
static void onKeyboardLeave(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface) { }
static void onKeyboardKey(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key,
    uint32_t state)
{
    logLine("key %u %u", key, state);
}
static void onKeyboardModifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t depressed,
    uint32_t latched, uint32_t locked, uint32_t group) { }
static void onKeyboardRepeatInfo(void* data, struct wl_keyboard* keyboard, int32_t rate, int32_t delay) { }
static const struct wl_keyboard_listener keyboardListener = {
    .keymap = onKeyboardKeymap, .enter = onKeyboardEnter, .leave = onKeyboardLeave, .key = onKeyboardKey,
    .modifiers = onKeyboardModifiers, .repeat_info = onKeyboardRepeatInfo,
};

static void onInhibitorActive(void* data, struct zwp_keyboard_shortcuts_inhibitor_v1* inhibitor)
{
    if (pointerLog) {
        logLine("inhibitor active");
    }
}
static void onInhibitorInactive(void* data, struct zwp_keyboard_shortcuts_inhibitor_v1* inhibitor)
{
    if (pointerLog) {
        logLine("inhibitor inactive");
    }
}
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener inhibitorListener = {
    .active = onInhibitorActive, .inactive = onInhibitorInactive,
};

static void onSeatCapabilities(void* data, struct wl_seat* s, uint32_t capabilities)
{
    static struct wl_pointer* pointer;
    static struct wl_keyboard* keyboard;
    static struct zwp_relative_pointer_v1* relativePointer;
    if (pointerLog && (capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointerListener, NULL);
    }
    if (pointer && relativePointerManager && !relativePointer) {
        relativePointer = zwp_relative_pointer_manager_v1_get_relative_pointer(relativePointerManager, pointer);
        zwp_relative_pointer_v1_add_listener(relativePointer, &relativePointerListener, NULL);
    }
    if (pointerLog && (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &keyboardListener, NULL);
    }
}
static void onSeatName(void* data, struct wl_seat* s, const char* name) { }
static const struct wl_seat_listener seatListener = { onSeatCapabilities, onSeatName };

static void onPreferredScale(void* data, struct wp_fractional_scale_v1* fractional, uint32_t scale)
{
    if (legacy || scale == scale120) {
        return;
    }
    scale120 = scale;
    if (configured) {
        draw();
    }
}

static const struct wp_fractional_scale_v1_listener fractionalListener = { onPreferredScale };

static void onPing(void* data, struct xdg_wm_base* base, uint32_t serial)
{
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wmBaseListener = { onPing };

static void onSurfaceConfigure(void* data, struct xdg_surface* xdg, uint32_t serial)
{
    xdg_surface_ack_configure(xdg, serial);
    if (pendingWidth > 0 && pendingHeight > 0) {
        logicalWidth = pendingWidth;
        logicalHeight = pendingHeight;
    }
    configured = 1;
    draw();
    if (popupWanted && !popupSurface) {
        openPopup();
    }
}

static const struct xdg_surface_listener surfaceListener = { onSurfaceConfigure };

static void onToplevelConfigure(void* data, struct xdg_toplevel* t, int32_t width, int32_t height, struct wl_array* states)
{
    pendingWidth = width;
    pendingHeight = height;
}

static void onToplevelClose(void* data, struct xdg_toplevel* t)
{
    running = 0;
}

static void onConfigureBounds(void* data, struct xdg_toplevel* t, int32_t width, int32_t height) { }
static void onWmCapabilities(void* data, struct xdg_toplevel* t, struct wl_array* capabilities) { }

static const struct xdg_toplevel_listener toplevelListener = {
    onToplevelConfigure, onToplevelClose, onConfigureBounds, onWmCapabilities,
};

int main(int argc, char** argv)
{
    int arg = 1;
    const char* appId = "vela.pattern";
    int decorated = 0;
    for (; arg < argc && !strncmp(argv[arg], "--", 2); ++arg) {
        if (!strcmp(argv[arg], "--scale1")) {
            legacy = 1;
        } else if (!strcmp(argv[arg], "--decorated")) {
            decorated = 1;
        } else if (!strcmp(argv[arg], "--inhibit-shortcuts")) {
            inhibitShortcuts = 1;
        } else if (!strcmp(argv[arg], "--popup") && arg + 1 < argc
            && sscanf(argv[arg + 1], "%d,%d", &popupX, &popupY) == 2) {
            popupWanted = 1;
            ++arg;
        } else if (!strcmp(argv[arg], "--pointer-log") && arg + 1 < argc) {
            pointerLog = fopen(argv[++arg], "w");
            if (!pointerLog) {
                perror("vela-pattern: --pointer-log");
                return 1;
            }
        } else if (!strcmp(argv[arg], "--app-id") && arg + 1 < argc) {
            // For tests that depend on the app (system dialogs).
            appId = argv[++arg];
        }
    }
    if (arg + 1 < argc) {
        logicalWidth = atoi(argv[arg]);
        logicalHeight = atoi(argv[arg + 1]);
    }
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "vela-pattern: no Wayland session (WAYLAND_DISPLAY)\n");
        return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registryListener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wmBase || !viewporter || !fractionalManager) {
        fprintf(stderr, "vela-pattern: missing protocols (viewporter, fractional-scale-v1)\n");
        return 1;
    }
    xdg_wm_base_add_listener(wmBase, &wmBaseListener, NULL);
    if (seat) {
        wl_seat_add_listener(seat, &seatListener, NULL);
    }

    surface = wl_compositor_create_surface(compositor);
    viewport = wp_viewporter_get_viewport(viewporter, surface);
    struct wp_fractional_scale_v1* fractional
        = wp_fractional_scale_manager_v1_get_fractional_scale(fractionalManager, surface);
    wp_fractional_scale_v1_add_listener(fractional, &fractionalListener, NULL);
    xdgSurface = xdg_wm_base_get_xdg_surface(wmBase, surface);
    xdg_surface_add_listener(xdgSurface, &surfaceListener, NULL);
    toplevel = xdg_surface_get_toplevel(xdgSurface);
    xdg_toplevel_add_listener(toplevel, &toplevelListener, NULL);
    xdg_toplevel_set_title(toplevel, "vela-pattern");
    xdg_toplevel_set_app_id(toplevel, appId);
    if (inhibitShortcuts) {
        if (!seat || !shortcutsInhibitManager) {
            fprintf(stderr, "vela-pattern: shortcut inhibition not supported\n");
            return 1;
        }
        struct zwp_keyboard_shortcuts_inhibitor_v1* inhibitor
            = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(shortcutsInhibitManager, surface, seat);
        zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inhibitor, &inhibitorListener, NULL);
    }
    if (decorated) {
        if (!decorationManager) {
            fprintf(stderr, "vela-pattern: no xdg-decoration\n");
            return 1;
        }
        struct zxdg_toplevel_decoration_v1* decoration
            = zxdg_decoration_manager_v1_get_toplevel_decoration(decorationManager, toplevel);
        zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(surface);

    while (running && wl_display_dispatch(display) != -1) {
    }
    return 0;
}
