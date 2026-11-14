// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-bigbuffer: a client with buffers as large as it likes, for the upload
// budget and image-extent tests (docs/memory-allocation-policy.md, point 6;
// tests/functional/test_uploads.py).
//
// Usage: vela-bigbuffer [--dmabuf] [--next-after MS] [--every-frame] [--app-id ID]
//                       WIDTH HEIGHT [WINDOW_WIDTH WINDOW_HEIGHT]
//
// Opens a window of WINDOW_WIDTH x WINDOW_HEIGHT (default 512x256; the
// buffer is scaled to it with wp_viewporter) and commits a shared-memory
// buffer of WIDTH x HEIGHT pixels (XRGB8888), damaged whole. Every version of
// the buffer is 16 horizontal bands: band b of version v is
//     R = 0x20 + 0x40 * (v % 4),  G = 0x08 + 0x10 * b,  B = 0x40
// so a frame that mixes two versions has bands of two reds, and a band in
// the wrong place has the wrong green.
//
// --next-after MS: MS after version 0 is released (copied), commits version 1.
// --every-frame: from version 1 on, commits a new version at every frame
//   callback, into whichever of two buffers the compositor has released.
// --dmabuf: instead, one linear dmabuf (GBM, on the compositor's device)
//   that claims to be WIDTH x HEIGHT: the memory behind it is as large as
//   that needs, whatever the device's limits; nothing is drawn in it.
//
// Prints "vela-bigbuffer: committed V" and "vela-bigbuffer: released V" on
// stderr.

#define _GNU_SOURCE
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xf86drm.h>

#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct xdg_wm_base* wmBase;
static struct wp_viewporter* viewporter;
static struct zwp_linux_dmabuf_v1* dmabufManager;
static dev_t mainDevice;
static int haveMainDevice;

static struct wl_surface* surface;
static int configured;
static int width, height;

struct buffer {
    struct wl_buffer* wl;
    uint32_t* pixels;
    int busy;
    int version;
};
static struct buffer buffers[2];
static int nextVersion = 1;
static int everyFrame;
static int nextAfterMs = -1;
static int64_t nextAt; // CLOCK_MONOTONIC ns; 0: not yet
static int finished;

static void die(const char* what)
{
    fprintf(stderr, "vela-bigbuffer: %s (%s)\n", what, strerror(errno));
    exit(1);
}

static int64_t now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void paint(struct buffer* b, int version)
{
    b->version = version;
    if (!b->pixels) return; // a dmabuf: nothing drawn in it
    for (int y = 0; y < height; ++y) {
        uint32_t band = (uint32_t)((int64_t)y * 16 / height);
        uint32_t color = 0xff000000u | (uint32_t)(0x20 + 0x40 * (version % 4)) << 16 | (0x08 + 0x10 * band) << 8
            | 0x40;
        uint32_t* row = b->pixels + (size_t)y * width;
        for (int x = 0; x < width; ++x) row[x] = color;
    }
}

static void onRelease(void* data, struct wl_buffer* wl)
{
    struct buffer* b = data;
    b->busy = 0;
    fprintf(stderr, "vela-bigbuffer: released %d\n", b->version);
    if (b->version == 0 && nextVersion == 1 && (nextAfterMs >= 0 || everyFrame) && !nextAt) {
        nextAt = now() + (int64_t)(nextAfterMs > 0 ? nextAfterMs : 0) * 1000000;
    }
}

static const struct wl_buffer_listener bufferListener = { onRelease };

static void commitBuffer(struct buffer* b);

static void onFrame(void* data, struct wl_callback* callback, uint32_t time);
static const struct wl_callback_listener frameListener = { onFrame };

static void commitBuffer(struct buffer* b)
{
    b->busy = 1;
    wl_surface_attach(surface, b->wl, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
    if (everyFrame) {
        wl_callback_add_listener(wl_surface_frame(surface), &frameListener, NULL);
    }
    wl_surface_commit(surface);
    fprintf(stderr, "vela-bigbuffer: committed %d\n", b->version);
}

static void commitNext(void)
{
    for (int i = 0; i < 2; ++i) {
        struct buffer* b = &buffers[(nextVersion + i) % 2];
        if (!b->busy) {
            paint(b, nextVersion++);
            commitBuffer(b);
            return;
        }
    }
    // Both still held: try again at the next frame.
    wl_callback_add_listener(wl_surface_frame(surface), &frameListener, NULL);
    wl_surface_commit(surface);
}

static void onFrame(void* data, struct wl_callback* callback, uint32_t time)
{
    wl_callback_destroy(callback);
    if (everyFrame && nextVersion > 1) {
        commitNext();
    }
}

static void shmBuffer(struct buffer* b)
{
    size_t size = (size_t)width * height * 4;
    if (size > INT32_MAX) {
        errno = EFBIG;
        die("a wl_shm pool is at most 2 GiB");
    }
    int fd = memfd_create("vela-bigbuffer", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) die("memfd");
    b->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (b->pixels == MAP_FAILED) die("mmap");
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    b->wl = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->wl, &bufferListener, b);
}

// A linear dmabuf claiming WIDTH x HEIGHT, backed by a GBM buffer with at
// least the bytes that layout needs.
static void dmabufBuffer(struct buffer* b)
{
    drmDevice* drm = NULL;
    if (!haveMainDevice || drmGetDeviceFromDevId(mainDevice, 0, &drm) || !(drm->available_nodes & (1 << DRM_NODE_RENDER))) {
        die("the compositor's render node wasn't found");
    }
    int drmFd = open(drm->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
    drmFreeDevice(&drm);
    if (drmFd < 0) die("opening the render node");
    struct gbm_device* gbm = gbm_create_device(drmFd);
    if (!gbm) die("gbm_create_device");
    uint32_t stride = ((uint32_t)width * 4 + 255) & ~255u;
    uint64_t bytes = (uint64_t)stride * (uint64_t)height;
    const uint32_t boWidth = 4096;
    uint32_t boHeight = (uint32_t)((bytes + boWidth * 4 - 1) / (boWidth * 4)) + 1;
    struct gbm_bo* bo = gbm_bo_create(gbm, boWidth, boHeight, GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR);
    if (!bo) die("gbm_bo_create");
    int fd = gbm_bo_get_fd(bo);
    if (fd < 0) die("gbm_bo_get_fd");
    struct zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(dmabufManager);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride, DRM_FORMAT_MOD_LINEAR >> 32,
        DRM_FORMAT_MOD_LINEAR & 0xffffffff);
    b->wl = zwp_linux_buffer_params_v1_create_immed(params, width, height, DRM_FORMAT_XRGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    wl_buffer_add_listener(b->wl, &bufferListener, b);
}

static void onFeedbackDone(void* data, struct zwp_linux_dmabuf_feedback_v1* f) { }
static void onFormatTable(void* data, struct zwp_linux_dmabuf_feedback_v1* f, int32_t fd, uint32_t size)
{
    close(fd);
}
static void onMainDevice(void* data, struct zwp_linux_dmabuf_feedback_v1* f, struct wl_array* device)
{
    if (device->size == sizeof(dev_t)) {
        memcpy(&mainDevice, device->data, sizeof(dev_t));
        haveMainDevice = 1;
    }
}
static void onTrancheDone(void* data, struct zwp_linux_dmabuf_feedback_v1* f) { }
static void onTrancheDevice(void* data, struct zwp_linux_dmabuf_feedback_v1* f, struct wl_array* device) { }
static void onTrancheFormats(void* data, struct zwp_linux_dmabuf_feedback_v1* f, struct wl_array* indices) { }
static void onTrancheFlags(void* data, struct zwp_linux_dmabuf_feedback_v1* f, uint32_t flags) { }
static const struct zwp_linux_dmabuf_feedback_v1_listener feedbackListener = {
    onFeedbackDone, onFormatTable, onMainDevice, onTrancheDone, onTrancheDevice, onTrancheFormats, onTrancheFlags,
};

static void onPing(void* data, struct xdg_wm_base* base, uint32_t serial)
{
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wmBaseListener = { onPing };

static void onSurfaceConfigure(void* data, struct xdg_surface* xdgSurface, uint32_t serial)
{
    xdg_surface_ack_configure(xdgSurface, serial);
    if (!configured) {
        configured = 1;
        paint(&buffers[0], 0);
        commitBuffer(&buffers[0]);
    } else {
        wl_surface_commit(surface);
    }
}
static const struct xdg_surface_listener xdgSurfaceListener = { onSurfaceConfigure };

static void onToplevelConfigure(void* data, struct xdg_toplevel* toplevel, int32_t w, int32_t h,
    struct wl_array* states) { }
static void onToplevelClose(void* data, struct xdg_toplevel* toplevel)
{
    finished = 1;
}
static void onBounds(void* data, struct xdg_toplevel* toplevel, int32_t w, int32_t h) { }
static void onCapabilities(void* data, struct xdg_toplevel* toplevel, struct wl_array* capabilities) { }
static const struct xdg_toplevel_listener toplevelListener = {
    onToplevelConfigure, onToplevelClose, onBounds, onCapabilities,
};

static void registryGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface,
    uint32_t version)
{
    if (!strcmp(interface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        wmBase = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name) && version >= 4) {
        dmabufManager = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 4);
    }
}
static void registryRemove(void* data, struct wl_registry* registry, uint32_t name) { }
static const struct wl_registry_listener registryListener = { registryGlobal, registryRemove };

int main(int argc, char** argv)
{
    int dmabuf = 0;
    const char* appId = "vela-bigbuffer";
    int sizes[4] = { 0, 0, 512, 256 };
    int sizeCount = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dmabuf")) {
            dmabuf = 1;
        } else if (!strcmp(argv[i], "--every-frame")) {
            everyFrame = 1;
        } else if (!strcmp(argv[i], "--next-after") && i + 1 < argc) {
            nextAfterMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--app-id") && i + 1 < argc) {
            appId = argv[++i];
        } else if (sizeCount < 4 && atoi(argv[i]) > 0) {
            sizes[sizeCount++] = atoi(argv[i]);
        } else {
            fprintf(stderr, "usage: vela-bigbuffer [--dmabuf] [--next-after MS] [--every-frame] [--app-id ID] "
                            "WIDTH HEIGHT [WINDOW_WIDTH WINDOW_HEIGHT]\n");
            return 2;
        }
    }
    if (sizeCount != 2 && sizeCount != 4) {
        fprintf(stderr, "vela-bigbuffer: WIDTH and HEIGHT are needed\n");
        return 2;
    }
    width = sizes[0];
    height = sizes[1];
    if (dmabuf && (nextAfterMs >= 0 || everyFrame)) {
        fprintf(stderr, "vela-bigbuffer: --dmabuf cannot be combined with --next-after or --every-frame\n");
        return 2;
    }

    struct wl_display* display = wl_display_connect(NULL);
    if (!display) die("no Wayland compositor");
    wl_registry_add_listener(wl_display_get_registry(display), &registryListener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !wmBase || !viewporter || (dmabuf && !dmabufManager)) {
        errno = ENOENT;
        die("missing wl_compositor, wl_shm, xdg_wm_base, wp_viewporter or linux-dmabuf v4");
    }
    if (dmabuf) {
        struct zwp_linux_dmabuf_feedback_v1* feedback = zwp_linux_dmabuf_v1_get_default_feedback(dmabufManager);
        zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &feedbackListener, NULL);
        wl_display_roundtrip(display);
        zwp_linux_dmabuf_feedback_v1_destroy(feedback);
        dmabufBuffer(&buffers[0]);
    } else {
        shmBuffer(&buffers[0]);
        if (nextAfterMs >= 0 || everyFrame) shmBuffer(&buffers[1]);
    }

    xdg_wm_base_add_listener(wmBase, &wmBaseListener, NULL);
    surface = wl_compositor_create_surface(compositor);
    struct wp_viewport* viewport = wp_viewporter_get_viewport(viewporter, surface);
    wp_viewport_set_destination(viewport, sizes[2], sizes[3]);
    struct xdg_surface* xdgSurface = xdg_wm_base_get_xdg_surface(wmBase, surface);
    xdg_surface_add_listener(xdgSurface, &xdgSurfaceListener, NULL);
    struct xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdgSurface);
    xdg_toplevel_add_listener(toplevel, &toplevelListener, NULL);
    xdg_toplevel_set_app_id(toplevel, appId);
    xdg_toplevel_set_title(toplevel, appId);
    wl_surface_commit(surface);

    while (!finished) {
        if (wl_display_dispatch_pending(display) < 0) break;
        wl_display_flush(display);
        if (nextAt && now() >= nextAt) {
            nextAt = 0;
            commitNext();
            wl_display_flush(display);
        }
        // Wake up at least every 5 ms, for --next-after.
        struct wl_display* d = display;
        if (wl_display_prepare_read(d) == 0) {
            struct timespec wait = { 0, 5000000 };
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(wl_display_get_fd(d), &fds);
            int ready = pselect(wl_display_get_fd(d) + 1, &fds, NULL, NULL, &wait, NULL);
            if (ready > 0) {
                if (wl_display_read_events(d) < 0) break;
            } else {
                wl_display_cancel_read(d);
            }
        }
    }
    wl_display_disconnect(display);
    return 0;
}
