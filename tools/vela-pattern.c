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
//                     [WIDTH HEIGHT]   (logical; default 401x301)
//
// --scale1: like an old app, always draws at scale 1 (the compositor must
// magnify): one-pixel lines and a checkerboard, to judge the filter.
// --decorated: asks for Vela's title bar (xdg-decoration), like Qt and KDE
// apps.
// --popup X,Y: also opens a 200x150 magenta menu (xdg-popup), anchored at
// point (X, Y) of the window; it may slide to stay on screen.
// --transient-delay MS: after the parent maps, opens another xdg_toplevel
// with xdg_toplevel.set_parent; used by multi-output functional tests.
// --transient-maximized: initially maximizes that child.
// --transient-siblings: create two overlapping siblings to test Z-order.
// --fullscreen-parent: request fullscreen for the parent before mapping.
// --log-pointer: writes "vela-pattern: pointer-button BUTTON STATE SURFACE" to
// stderr for every button event, SURFACE being "window" or "popup" (where
// the clicks go).
// --lock-pointer: locks the pointer (pointer-constraints) once it enters the
// window, as games do, and logs "vela-pattern: pointer-locked" and
// "vela-pattern: pointer-unlocked"; implies --log-pointer.

#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fractional-scale-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
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

static struct wl_display* connection;
struct transient_window {
    struct wl_surface* surface;
    int width, height;
    uint32_t color;
};

static struct transient_window transientWindows[2] = {
    { .width = 320, .height = 240, .color = 0xffaa33ccu },
    { .width = 320, .height = 240, .color = 0xff33cc66u },
};
static int transientDelayMs = -1;
static int transientMaximized;
static int transientSiblings;
static int fullscreenParent;
static int logPointer;
static int lockPointer;
static struct wl_seat* seat;
static struct zwp_pointer_constraints_v1* pointerConstraints;
static struct zwp_locked_pointer_v1* lockedPointer;

static struct wl_surface* pointerSurface; // the one the pointer entered

static void onLocked(void* data, struct zwp_locked_pointer_v1* locked)
{
    fprintf(stderr, "vela-pattern: pointer-locked\n");
}
static void onUnlocked(void* data, struct zwp_locked_pointer_v1* locked)
{
    fprintf(stderr, "vela-pattern: pointer-unlocked\n");
}
static const struct zwp_locked_pointer_v1_listener lockedListener = { onLocked, onUnlocked };

static void onPointerEnter(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* entered,
    wl_fixed_t x, wl_fixed_t y)
{
    pointerSurface = entered;
    // Like a game: once the pointer is on the window, it stays there.
    if (lockPointer && pointerConstraints && !lockedPointer && entered == surface) {
        lockedPointer = zwp_pointer_constraints_v1_lock_pointer(pointerConstraints, surface, pointer, NULL,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_add_listener(lockedPointer, &lockedListener, NULL);
    }
}
static void onPointerLeave(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface)
{
    pointerSurface = NULL;
}
static void onPointerMotion(void* data, struct wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y) { }
static void onPointerButton(void* data, struct wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button,
    uint32_t state)
{
    const char* where = !pointerSurface ? "none" : pointerSurface == popupSurface ? "popup" : "window";
    fprintf(stderr, "vela-pattern: pointer-button %u %u %s\n", button, state, where);
}
static void onPointerAxis(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value) { }

static const struct wl_pointer_listener pointerListener = {
    .enter = onPointerEnter,
    .leave = onPointerLeave,
    .motion = onPointerMotion,
    .button = onPointerButton,
    .axis = onPointerAxis,
};

static void onSeatCapabilities(void* data, struct wl_seat* seat, uint32_t capabilities)
{
    static struct wl_pointer* pointer;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointerListener, NULL);
    }
}

static const struct wl_seat_listener seatListener = { .capabilities = onSeatCapabilities };

static void onGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
{
    if (logPointer && !seat && !strcmp(interface, wl_seat_interface.name)) {
        // Version 1: no pointer frames to answer.
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seatListener, NULL);
    } else if (lockPointer && !strcmp(interface, zwp_pointer_constraints_v1_interface.name)) {
        pointerConstraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
    } else if (!strcmp(interface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
        decorationManager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        wmBase = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
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

// A second xdg_toplevel, not an xdg_popup: exercises initial placement
// against the mapped parent's monitor, and the XDG configure handshake.
static void drawTransient(struct transient_window* window)
{
    const int width = window->width, height = window->height;
    const int stride = width * 4;
    const size_t size = (size_t)stride * height;
    const int fd = memfd_create("vela-pattern-transient", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        perror("vela-pattern: transient memfd");
        exit(1);
    }
    uint32_t* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        perror("vela-pattern: transient mmap");
        exit(1);
    }
    for (int i = 0; i < width * height; ++i) {
        pixels[i] = window->color;
    }
    munmap(pixels, size);
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(buffer, &bufferListener, NULL);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(window->surface, buffer, 0, 0);
    wl_surface_damage_buffer(window->surface, 0, 0, width, height);
    wl_surface_commit(window->surface);
}

static void onTransientSurfaceConfigure(void* data, struct xdg_surface* xdg, uint32_t serial)
{
    xdg_surface_ack_configure(xdg, serial);
    drawTransient(data);
}
static const struct xdg_surface_listener transientSurfaceListener = { onTransientSurfaceConfigure };

static void onTransientConfigure(void* data, struct xdg_toplevel* t, int32_t width, int32_t height,
    struct wl_array* states)
{
    struct transient_window* window = data;
    if (width > 0 && height > 0) {
        window->width = width;
        window->height = height;
    }
}
static void onTransientClose(void* data, struct xdg_toplevel* t) { running = 0; }
static const struct xdg_toplevel_listener transientToplevelListener = {
    .configure = onTransientConfigure,
    .close = onTransientClose,
};

static void openTransient(int index)
{
    struct transient_window* window = &transientWindows[index];
    window->surface = wl_compositor_create_surface(compositor);
    struct xdg_surface* xdg = xdg_wm_base_get_xdg_surface(wmBase, window->surface);
    xdg_surface_add_listener(xdg, &transientSurfaceListener, window);
    struct xdg_toplevel* child = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(child, &transientToplevelListener, window);
    xdg_toplevel_set_parent(child, toplevel);
    xdg_toplevel_set_title(child, index ? "vela-pattern-transient-2" : "vela-pattern-transient");
    xdg_toplevel_set_app_id(child, index ? "vela.pattern.transient.2" : "vela.pattern.transient");
    if (transientMaximized) {
        xdg_toplevel_set_maximized(child);
    }
    wl_surface_commit(window->surface);
}

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
    if (transientDelayMs >= 0 && !transientWindows[0].surface) {
        // Let the parent map so the harness can move it to another output.
        wl_display_flush(connection);
        if (transientDelayMs > 0) {
            usleep((useconds_t)transientDelayMs * 1000);
        }
        openTransient(0);
        if (transientSiblings) {
            openTransient(1);
        }
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
        } else if (!strcmp(argv[arg], "--popup") && arg + 1 < argc
            && sscanf(argv[arg + 1], "%d,%d", &popupX, &popupY) == 2) {
            popupWanted = 1;
            ++arg;
        } else if (!strcmp(argv[arg], "--transient-delay") && arg + 1 < argc) {
            transientDelayMs = atoi(argv[++arg]);
            if (transientDelayMs < 0 || transientDelayMs > 15000) {
                fprintf(stderr, "vela-pattern: transient delay must be 0..15000 ms\n");
                return 1;
            }
        } else if (!strcmp(argv[arg], "--transient-maximized")) {
            transientMaximized = 1;
        } else if (!strcmp(argv[arg], "--transient-siblings")) {
            transientSiblings = 1;
        } else if (!strcmp(argv[arg], "--log-pointer")) {
            logPointer = 1;
        } else if (!strcmp(argv[arg], "--lock-pointer")) {
            logPointer = lockPointer = 1;
        } else if (!strcmp(argv[arg], "--fullscreen-parent")) {
            fullscreenParent = 1;
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
    connection = display;
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
    if (fullscreenParent) {
        xdg_toplevel_set_fullscreen(toplevel, NULL);
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
