// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vela/capture.h"
#include <stdint.h>
#include <stdio.h>
#include <wayland-client.h>

/** Runtime checks must remain active in RelWithDebInfo/-DNDEBUG builds. */
static int check(bool actual, bool expected, const char *name)
{
    if (actual != expected) {
        fprintf(stderr, "capture-frame-bounds: %s failed\n", name);
        return 1;
    }
    return 0;
}

/** A rectangular, padded buffer distinguishes all eight rotations and flips. */
static int check_transforms(void)
{
    const uint8_t storage[32] = {
        1, 11, 21, 31, 2, 12, 22, 32, 3, 13, 23, 33, 99, 99, 99, 99,
        4, 14, 24, 34, 5, 15, 25, 35, 6, 16, 26, 36, 99, 99, 99, 99,
    };
    const uint8_t expected[8][6] = {
        {1, 2, 3, 4, 5, 6}, {4, 1, 5, 2, 6, 3},
        {6, 5, 4, 3, 2, 1}, {3, 6, 2, 5, 1, 4},
        {3, 2, 1, 6, 5, 4}, {1, 4, 2, 5, 3, 6},
        {4, 5, 6, 1, 2, 3}, {6, 3, 5, 2, 4, 1},
    };
    struct vela_capture_frame frame = {
        .pixels = storage, .length = sizeof(storage), .width = 3, .height = 2,
        .stride = 16, .format = WL_SHM_FORMAT_XRGB8888,
    };
    int failures = 0;
    for (uint32_t transform = 0; transform < 8; ++transform) {
        frame.transform = transform;
        uint32_t width = 0, height = 0;
        failures += check(vela_capture_frame_dimensions(&frame, &width, &height), true, "oriented size");
        const uint32_t wanted_width = transform & 1 ? 2 : 3;
        const uint32_t wanted_height = transform & 1 ? 3 : 2;
        failures += check(width == wanted_width && height == wanted_height, true, "swapped dimensions");
        for (uint32_t y = 0; y < wanted_height; ++y) {
            for (uint32_t x = 0; x < wanted_width; ++x) {
                const uint8_t *pixel = vela_capture_frame_pixel(&frame, x, y);
                uint8_t value = expected[transform][y * wanted_width + x];
                failures += check(pixel && pixel[0] == value && pixel[1] == value + 10
                    && pixel[2] == value + 20 && pixel[3] == value + 30, true, "oriented pixel");
            }
        }
        failures += check(vela_capture_frame_pixel(&frame, wanted_width, 0) == NULL, true, "right boundary");
        failures += check(vela_capture_frame_pixel(&frame, 0, wanted_height) == NULL, true, "bottom boundary");
        failures += check(vela_capture_frame_pixel(&frame, UINT32_MAX, UINT32_MAX) == NULL, true, "huge coordinates");
    }
    uint32_t width, height;
    frame.transform = 8;
    failures += check(vela_capture_frame_dimensions(&frame, &width, &height), false, "unknown transform");
    failures += check(vela_capture_frame_pixel(&frame, 0, 0) == NULL, true, "unknown transform pixel");
    frame.transform = 0;
    frame.length = 31;
    failures += check(vela_capture_frame_pixel(&frame, 0, 0) == NULL, true, "short transformed buffer");
    failures += check(vela_capture_frame_dimensions(NULL, &width, &height), false, "null oriented frame");
    failures += check(vela_capture_frame_dimensions(&frame, NULL, &height), false, "null size pointer");
    return failures;
}

/** Validate SHM layout and overflow guards without compositor or GPU. */
int main(void)
{
    const uint8_t storage[64] = {0};
    struct vela_capture_frame frame = {
        .pixels = storage, .length = 64, .width = 4, .height = 4,
        .stride = 16, .format = WL_SHM_FORMAT_XRGB8888,
    };
    int failures = check(vela_capture_frame_valid(&frame), true, "valid");
    failures += check(vela_capture_frame_valid(NULL), false, "null");
    frame.length = 63;
    failures += check(vela_capture_frame_valid(&frame), false, "short allocation");
    frame.length = 64;
    frame.stride = 12;
    failures += check(vela_capture_frame_valid(&frame), false, "short stride");
    frame.stride = 16;
    frame.width = 5;
    failures += check(vela_capture_frame_valid(&frame), false, "width overflow");
    frame.width = 4;
    frame.height = 5;
    failures += check(vela_capture_frame_valid(&frame), false, "height overflow");
    frame.height = 4;
    frame.width = UINT32_MAX;
    failures += check(vela_capture_frame_valid(&frame), false, "huge width");
    frame.width = 4;
    frame.format = UINT32_MAX;
    failures += check(vela_capture_frame_valid(&frame), false, "unsupported format");
    frame.format = WL_SHM_FORMAT_XRGB8888;
    frame.pixels = NULL;
    failures += check(vela_capture_frame_valid(&frame), false, "null pixels");
    failures += check_transforms();
    return failures ? 1 : 0;
}
