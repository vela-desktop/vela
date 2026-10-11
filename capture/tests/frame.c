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
    return failures ? 1 : 0;
}
