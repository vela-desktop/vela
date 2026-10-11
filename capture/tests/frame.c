// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vela/capture.h"
#include <assert.h>
#include <stdint.h>
#include <wayland-client.h>

/** Validate SHM layout and overflow guards without compositor or GPU. */
int main(void)
{
    const uint8_t storage[64] = {0};
    struct vela_capture_frame frame = {
        .pixels = storage, .length = 64, .width = 4, .height = 4,
        .stride = 16, .format = WL_SHM_FORMAT_XRGB8888,
    };
    assert(vela_capture_frame_valid(&frame));
    assert(!vela_capture_frame_valid(NULL));
    frame.length = 63;
    assert(!vela_capture_frame_valid(&frame));
    frame.length = 64;
    frame.stride = 12;
    assert(!vela_capture_frame_valid(&frame));
    frame.stride = 16;
    frame.width = 5;
    assert(!vela_capture_frame_valid(&frame));
    frame.width = 4;
    frame.height = 5;
    assert(!vela_capture_frame_valid(&frame));
    frame.height = 4;
    frame.width = UINT32_MAX;
    assert(!vela_capture_frame_valid(&frame));
    frame.width = 4;
    frame.format = UINT32_MAX;
    assert(!vela_capture_frame_valid(&frame));
    frame.format = WL_SHM_FORMAT_XRGB8888;
    frame.pixels = NULL;
    assert(!vela_capture_frame_valid(&frame));
    return 0;
}
