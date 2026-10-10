// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-shot: captures Vela's screen to PNG (wlr-screencopy protocol).
//
// Usage: vela-shot FILE.png [X Y WIDTH HEIGHT]
//
// Captures the first output; with coordinates, only that area (in output
// pixels). Uses the session given by WAYLAND_DISPLAY.

#define _GNU_SOURCE
#include <stdint.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>
#include <zlib.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"

static struct wl_shm* shm;
static struct wl_output* output;
static struct zwlr_screencopy_manager_v1* manager;

static struct {
    uint32_t format, width, height, stride;
    int haveBuffer, ready, failed, yInvert;
} frame;

static void registryGlobal(void* data, struct wl_registry* registry, uint32_t name,
    const char* interface, uint32_t version)
{
    if (!strcmp(interface, wl_shm_interface.name)) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, wl_output_interface.name) && !output) {
        // VELA_SHOT_OUTPUT=N picks the N-th output (from 0); otherwise the
        // first.
        static int seen = 0;
        const char* wanted = getenv("VELA_SHOT_OUTPUT");
        if (seen++ == (wanted ? atoi(wanted) : 0)) {
            output = wl_registry_bind(registry, name, &wl_output_interface, 1);
        }
    } else if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
        manager = wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 1);
    }
}

static void registryRemove(void* data, struct wl_registry* registry, uint32_t name) { }

static const struct wl_registry_listener registryListener = { registryGlobal, registryRemove };

static void frameBuffer(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t format,
    uint32_t width, uint32_t height, uint32_t stride)
{
    frame.format = format;
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.haveBuffer = 1;
}

static void frameFlags(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t flags)
{
    frame.yInvert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frameReady(void* data, struct zwlr_screencopy_frame_v1* f, uint32_t hi, uint32_t lo, uint32_t ns)
{
    frame.ready = 1;
}

static void frameFailed(void* data, struct zwlr_screencopy_frame_v1* f)
{
    frame.failed = 1;
}

static const struct zwlr_screencopy_frame_v1_listener frameListener = {
    .buffer = frameBuffer,
    .flags = frameFlags,
    .ready = frameReady,
    .failed = frameFailed,
};

// ------------------------------------------------------------------ PNG --

static void put32(unsigned char* p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void writeChunk(FILE* file, const char* type, const unsigned char* data, uint32_t length)
{
    unsigned char header[8];
    put32(header, length);
    memcpy(header + 4, type, 4);
    fwrite(header, 1, 8, file);
    if (length) {
        fwrite(data, 1, length, file);
    }
    uLong crc = crc32(0, (const Bytef*)type, 4);
    crc = crc32(crc, data, length);
    unsigned char trailer[4];
    put32(trailer, (uint32_t)crc);
    fwrite(trailer, 1, 4, file);
}

// Output pixels -> 8-bit RGB PNG.
static int writePng(const char* path, const unsigned char* pixels, int x, int y, int w, int h)
{
    // Bound both allocation arithmetic and zlib's input length.
    if (w <= 0 || h <= 0 || (size_t)w > (SIZE_MAX - 1) / 3) {
        return 0;
    }
    const size_t rowSize = 1 + (size_t)w * 3;
    if ((size_t)h > SIZE_MAX / rowSize || (size_t)h > ULONG_MAX / rowSize) {
        return 0;
    }
    const size_t rawSize = rowSize * (size_t)h;
    unsigned char* raw = malloc(rawSize);
    if (!raw) {
        return 0;
    }
    // In memory: XRGB/ARGB8888 = B,G,R,A; XBGR/ABGR8888 = R,G,B,A.
    const int bgr = frame.format == WL_SHM_FORMAT_XRGB8888 || frame.format == WL_SHM_FORMAT_ARGB8888;
    for (int row = 0; row < h; ++row) {
        const int srcRow = frame.yInvert ? (int)frame.height - 1 - (y + row) : y + row;
        const unsigned char* src = pixels + (size_t)srcRow * frame.stride + (size_t)x * 4;
        unsigned char* dst = raw + (size_t)row * rowSize;
        *dst++ = 0; // filter: none
        for (int col = 0; col < w; ++col, src += 4) {
            *dst++ = bgr ? src[2] : src[0];
            *dst++ = src[1];
            *dst++ = bgr ? src[0] : src[2];
        }
    }
    const uLongf bound = compressBound((uLong)rawSize);
    if (!bound || bound > UINT32_MAX) {
        free(raw);
        return 0;
    }
    unsigned char* packed = malloc((size_t)bound);
    if (!packed) {
        free(raw);
        return 0;
    }
    uLongf packedSize = bound;
    int compressed = compress2(packed, &packedSize, raw, (uLong)rawSize, 6);
    free(raw);
    if (compressed != Z_OK || packedSize > UINT32_MAX) {
        free(packed);
        return 0;
    }

    // A screenshot can contain secrets. Do not follow symlinks, and ensure
    // even an existing output file is owner-readable/writable only.
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        perror(path);
        free(packed);
        return 0;
    }
    if (fchmod(fd, S_IRUSR | S_IWUSR) != 0 || ftruncate(fd, 0) != 0) {
        perror(path);
        close(fd);
        free(packed);
        return 0;
    }
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        perror(path);
        close(fd);
        free(packed);
        return 0;
    }
    static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    fwrite(signature, 1, 8, file);
    unsigned char ihdr[13];
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = 8; // bits per channel
    ihdr[9] = 2; // RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    writeChunk(file, "IHDR", ihdr, sizeof(ihdr));
    writeChunk(file, "IDAT", packed, (uint32_t)packedSize);
    writeChunk(file, "IEND", NULL, 0);
    int ok = !ferror(file);
    ok = fclose(file) == 0 && ok;
    free(packed);
    return ok;
}

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 6) {
        fprintf(stderr, "Usage: %s FILE.png [X Y WIDTH HEIGHT]\n", argv[0]);
        return 2;
    }
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "vela-shot: no Wayland session (WAYLAND_DISPLAY)\n");
        return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registryListener, NULL);
    wl_display_roundtrip(display);
    if (!manager || !shm || !output) {
        fprintf(stderr, "vela-shot: the compositor doesn't offer wlr-screencopy\n");
        return 1;
    }

    struct zwlr_screencopy_frame_v1* capture = zwlr_screencopy_manager_v1_capture_output(manager, 0, output);
    zwlr_screencopy_frame_v1_add_listener(capture, &frameListener, NULL);
    while (!frame.haveBuffer && !frame.failed && wl_display_dispatch(display) != -1) { }
    if (frame.failed) {
        fprintf(stderr, "vela-shot: capture refused\n");
        return 1;
    }

    const size_t size = (size_t)frame.stride * frame.height;
    const int fd = memfd_create("vela-shot", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        perror("vela-shot");
        return 1;
    }
    unsigned char* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, frame.width, frame.height,
        frame.stride, frame.format);
    zwlr_screencopy_frame_v1_copy(capture, buffer);
    while (!frame.ready && !frame.failed && wl_display_dispatch(display) != -1) { }
    if (frame.failed) {
        fprintf(stderr, "vela-shot: copy failed\n");
        return 1;
    }

    int x = 0, y = 0, w = frame.width, h = frame.height;
    if (argc == 6) {
        x = atoi(argv[2]);
        y = atoi(argv[3]);
        w = atoi(argv[4]);
        h = atoi(argv[5]);
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > (int)frame.width || y + h > (int)frame.height) {
            fprintf(stderr, "vela-shot: region outside the output (%ux%u)\n", frame.width, frame.height);
            return 2;
        }
    }
    const int ok = writePng(argv[1], pixels, x, y, w, h);
    wl_display_disconnect(display);
    return ok ? 0 : 1;
}
