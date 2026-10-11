// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-screen-dump: captures an output to PNG through the shared C capture core.
//
// Usage: vela-screen-dump FILE.png [X Y WIDTH HEIGHT]
//
// Captures the first output; with coordinates, only that area (in output
// pixels). Uses the session given by WAYLAND_DISPLAY.

#define _GNU_SOURCE
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <zlib.h>

#include "vela/capture.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

static struct wl_shm* shm;
static struct wl_output* output;
static struct ext_output_image_capture_source_manager_v1* sources;
static struct ext_image_copy_capture_manager_v1* manager;
static struct vela_capture_job* capture;
static bool done, capture_ok;
static const struct vela_capture_frame* result;

static void captured(struct vela_capture_job* job, bool ok, void* userdata)
{
    (void)userdata;
    capture_ok = ok;
    result = vela_capture_pixels(job);
    done = true;
}

static void registryGlobal(void* data, struct wl_registry* registry, uint32_t name,
    const char* interface, uint32_t version)
{
    (void)data; (void)version;
    if (!strcmp(interface, wl_shm_interface.name)) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        static int seen = 0;
        const char* wanted = getenv("VELA_SCREEN_DUMP_OUTPUT");
        if (seen++ == (wanted ? atoi(wanted) : 0)) {
            output = wl_registry_bind(registry, name, &wl_output_interface, 1);
        }
    } else if (!strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
        sources = wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
        manager = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    }
}

static void registryRemove(void* data, struct wl_registry* registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}
static const struct wl_registry_listener registryListener = { registryGlobal, registryRemove };

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
static int writePng(const char* path, const struct vela_capture_frame* frame, int x, int y, int w, int h)
{
    // In memory: XRGB/ARGB8888 = B,G,R,A; XBGR/ABGR8888 = R,G,B,A.
    const unsigned char* pixels = frame->pixels;
    const int bgr = frame->format == WL_SHM_FORMAT_XRGB8888 || frame->format == WL_SHM_FORMAT_ARGB8888;
    const size_t rowSize = 1 + (size_t)w * 3;
    unsigned char* raw = malloc(rowSize * h);
    for (int row = 0; row < h; ++row) {
        const int srcRow = y + row;
        const unsigned char* src = pixels + (size_t)srcRow * frame->stride + (size_t)x * 4;
        unsigned char* dst = raw + row * rowSize;
        *dst++ = 0; // filter: none
        for (int col = 0; col < w; ++col, src += 4) {
            *dst++ = bgr ? src[2] : src[0];
            *dst++ = src[1];
            *dst++ = bgr ? src[0] : src[2];
        }
    }
    uLongf packedSize = compressBound(rowSize * h);
    unsigned char* packed = malloc(packedSize);
    compress2(packed, &packedSize, raw, rowSize * h, 6);

    FILE* file = fopen(path, "wb");
    if (!file) {
        perror(path);
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
    writeChunk(file, "IDAT", packed, packedSize);
    writeChunk(file, "IEND", NULL, 0);
    fclose(file);
    free(raw);
    free(packed);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 6) {
        fprintf(stderr, "Usage: %s FILE.png [X Y WIDTH HEIGHT]\n", argv[0]);
        return 2;
    }
    struct wl_display* display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "vela-screen-dump: no Wayland session (WAYLAND_DISPLAY)\n");
        return 1;
    }
    wl_registry_add_listener(wl_display_get_registry(display), &registryListener, NULL);
    wl_display_roundtrip(display);
    if (!manager || !shm || !output || !sources) {
        fprintf(stderr, "vela-screen-dump: the compositor doesn't offer ext-image-copy-capture\n");
        return 1;
    }

    struct ext_image_capture_source_v1* source =
        ext_output_image_capture_source_manager_v1_create_source(sources, output);
    capture = vela_capture_begin(shm, manager, source, captured, NULL);
    if (!capture) {
        if (source) ext_image_capture_source_v1_destroy(source);
        fprintf(stderr, "vela-screen-dump: failed to create capture session\n");
        return 1;
    }
    wl_display_flush(display);
    while (!done) {
        if (wl_display_dispatch(display) < 0) {
            fprintf(stderr, "vela-screen-dump: Wayland connection lost\n");
            vela_capture_end(capture);
            return 1;
        }
    }
    if (!capture_ok || !result || !result->pixels || result->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
        fprintf(stderr, "vela-screen-dump: capture failed or transformed output unsupported\n");
        vela_capture_end(capture);
        return 1;
    }

    int x = 0, y = 0, w = result->width, h = result->height;
    if (argc == 6) {
        x = atoi(argv[2]);
        y = atoi(argv[3]);
        w = atoi(argv[4]);
        h = atoi(argv[5]);
        if (x < 0 || y < 0 || w <= 0 || h <= 0 || (int64_t)x + w > result->width || (int64_t)y + h > result->height) {
            fprintf(stderr, "vela-screen-dump: region outside the output (%ux%u)\n", result->width, result->height);
            return 2;
        }
    }
    const int ok = writePng(argv[1], result, x, y, w, h);
    vela_capture_end(capture);
    wl_display_disconnect(display);
    return ok ? 0 : 1;
}
