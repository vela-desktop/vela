// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// GPU test client: padded ARGB destinations, concurrent frames, and explicit
// destruction while a copy is pending. "hold" waits for stdin before teardown.
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

struct capture {
    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;
    struct wl_buffer *buffer;
    void *pixels;
    uint32_t width, height, stride;
    size_t size;
    bool terminal;
    int rounds;
};

static struct wl_shm *shm;
static struct wl_output *output;
static struct ext_output_image_capture_source_manager_v1 *sources;
static struct ext_image_copy_capture_manager_v1 *manager;
static bool hold, failed, repeat, overflow;
static int successes, rejections;
static void session_done(void *data, struct ext_image_copy_capture_session_v1 *session);
static int terminal, selected_output;

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t transform) {}
static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame,
    int32_t x, int32_t y, int32_t width, int32_t height) {}
static void frame_time(void *data, struct ext_image_copy_capture_frame_v1 *frame,
    uint32_t hi, uint32_t lo, uint32_t nsec) {}

static void finish(struct capture *capture, bool ok)
{
    if (capture->terminal) {
        failed = true;
        return;
    }
    capture->terminal = true;
    ++terminal;
    if (ok) ++successes; else ++rejections;
    if (!hold && !overflow && !ok) failed = true;
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame)
{
    struct capture *capture = data;
    bool ok = true;
    if (!hold && !overflow) {
        for (uint32_t y = 0; y < capture->height; ++y) {
            const uint8_t *row = (uint8_t *)capture->pixels + (size_t)y * capture->stride;
            for (uint32_t x = 0; x < capture->width; ++x) {
                // The pointer is parked at (0,0); ignore its small footprint.
                if (x < 64 && y < 64) continue;
                const uint8_t *p = row + (size_t)x * 4;
                // Independent source oracle: opaque #123456; a channel swap
                // must fail even if both screenshots agree with each other.
                if (p[0] != 0x56 || p[1] != 0x34 || p[2] != 0x12 || p[3] != 0xff) {
                    fprintf(stderr, "pixel %u,%u: %u,%u,%u,%u\n", x, y, p[0], p[1], p[2], p[3]);
                    ok = false;
                    goto checked;
                }
            }
            for (uint32_t x = capture->width * 4; x < capture->stride; ++x) {
                if (row[x] != 0x5a) {
                    fprintf(stderr, "row %u padding was overwritten\n", y);
                    ok = false;
                    goto checked;
                }
            }
        }
    }
checked:
    if (ok && repeat) ++capture->rounds;
    if (ok && repeat && capture->rounds < 3) {
        // Keep the same protocol session on an unchanged output. A lost-damage
        // bug would leave the next capture waiting forever.
        ext_image_copy_capture_frame_v1_destroy(capture->frame);
        wl_buffer_destroy(capture->buffer);
        munmap(capture->pixels, capture->size);
        capture->frame = NULL;
        capture->buffer = NULL;
        capture->pixels = NULL;
        session_done(capture, capture->session);
    } else {
        finish(capture, ok);
    }
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason)
{
    fprintf(stderr, "capture failed: %u\n", reason);
    finish(data, false);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    frame_transform, frame_damage, frame_time, frame_ready, frame_failed,
};

static void buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t width, uint32_t height)
{
    struct capture *capture = data;
    if (!capture->frame) {
        capture->width = width;
        capture->height = height;
    }
}
static void shm_format(void *data, struct ext_image_copy_capture_session_v1 *session, uint32_t format) {}
static void dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session, struct wl_array *dev) {}
static void dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session,
    uint32_t format, struct wl_array *modifiers) {}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *session)
{
    struct capture *capture = data;
    if (capture->frame || capture->terminal) return;
    if (!capture->width || !capture->height || capture->width > (INT32_MAX - 64) / 4) goto error;
    capture->stride = capture->width * 4 + 64;
    if (capture->height > INT32_MAX / capture->stride) goto error;
    capture->size = (size_t)capture->stride * capture->height;
    int fd = memfd_create("vela-capture-probe", MFD_CLOEXEC);
    if (fd < 0) goto error;
    if (ftruncate(fd, (off_t)capture->size) < 0) {
        close(fd);
        goto error;
    }
    capture->pixels = mmap(NULL, capture->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (capture->pixels == MAP_FAILED) {
        capture->pixels = NULL;
        close(fd);
        goto error;
    }
    // Pixel tests exercise row padding and XRGB->ARGB opaque alpha. Hold mode
    // leaves the new destination untouched, matching ordinary screenshots.
    if (!hold) memset(capture->pixels, 0x5a, capture->size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)capture->size);
    capture->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)capture->width,
        (int32_t)capture->height, (int32_t)capture->stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    capture->frame = ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(capture->frame, &frame_listener, capture);
    ext_image_copy_capture_frame_v1_attach_buffer(capture->frame, capture->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(capture->frame, 0, 0,
        (int32_t)capture->width, (int32_t)capture->height);
    ext_image_copy_capture_frame_v1_capture(capture->frame);
    return;
error:
    finish(capture, false);
}
static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *session)
{
    struct capture *capture = data;
    if (!capture->terminal) finish(capture, false);
}
static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    buffer_size, shm_format, dmabuf_device, dmabuf_format, session_done, session_stopped,
};

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    static int seen;
    if (!strcmp(interface, wl_shm_interface.name)) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        if (seen++ == selected_output) output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    } else if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
        sources = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
        manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    }
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = { global, removed };

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    hold = !strcmp(argv[1], "hold");
    repeat = !strcmp(argv[1], "repeat");
    overflow = !strcmp(argv[1], "overflow");
    int count = atoi(argv[2]);
    if (count < 1 || count > 8) return 2;
    selected_output = argc > 3 ? atoi(argv[3]) : 0;
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) return 1;
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !shm || !output || !sources || !manager) return 1;
    struct capture captures[8] = { 0 };
    for (int i = 0; i < count; ++i) {
        captures[i].source = ext_output_image_capture_source_manager_v1_create_source(sources, output);
        captures[i].session = ext_image_copy_capture_manager_v1_create_session(manager, captures[i].source, 0);
        ext_image_copy_capture_session_v1_add_listener(captures[i].session, &session_listener, &captures[i]);
    }
    bool stop = false;
    while (!stop && (hold || terminal < count)) {
        while (wl_display_prepare_read(display) < 0) {
            if (wl_display_dispatch_pending(display) < 0) return 1;
        }
        wl_display_flush(display);
        struct pollfd fds[] = {
            { .fd = wl_display_get_fd(display), .events = POLLIN },
            { .fd = hold ? STDIN_FILENO : -1, .events = POLLIN },
        };
        int n = poll(fds, 2, 5000);
        if (n > 0 && (fds[0].revents & POLLIN)) {
            if (wl_display_read_events(display) < 0) return 1;
        } else {
            wl_display_cancel_read(display);
        }
        if (n <= 0 || (fds[0].revents & (POLLERR | POLLHUP))) return 1;
        if (fds[1].revents) stop = true;
        if (wl_display_dispatch_pending(display) < 0) return 1;
    }
    for (int i = 0; i < count; ++i) {
        struct capture *capture = &captures[i];
        if (capture->frame) ext_image_copy_capture_frame_v1_destroy(capture->frame);
        ext_image_copy_capture_session_v1_destroy(capture->session);
        ext_image_capture_source_v1_destroy(capture->source);
        if (capture->buffer) wl_buffer_destroy(capture->buffer);
    }
    // Prove explicit frame destruction without closing the connection first.
    if (wl_display_roundtrip(display) < 0) return 1;
    for (int i = 0; i < count; ++i) {
        if (captures[i].pixels) munmap(captures[i].pixels, captures[i].size);
    }
    wl_display_disconnect(display);
    if (overflow && (successes != 4 || rejections != 4)) return 1;
    return failed ? 1 : 0;
}
