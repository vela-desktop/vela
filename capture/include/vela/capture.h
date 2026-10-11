// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_shm;
struct ext_image_capture_source_v1;
struct ext_image_copy_capture_manager_v1;
struct vela_capture_job;

// Pixels belong to the job and are valid only during the completion callback
// and until vela_capture_end(). All dimensions/stride are in buffer pixels.
// The caller converts pixels to a UI image or writes them to a file.
struct vela_capture_frame {
    const uint8_t *pixels;
    size_t length;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;    // wl_shm format
    uint32_t transform; // wl_output_transform reported by the protocol
};

typedef void (*vela_capture_done_fn)(struct vela_capture_job *job, bool success, void *userdata);

// Start an asynchronous ext-image-copy-capture-v1 SHM capture. The caller owns
// its wl_display/event dispatch. On success this job owns `source` and calls
// `done` exactly once (including failed/stopped sessions). If NULL is
// returned, ownership remains with the caller.
//
// The library does not create a new Wayland connection or dispatch events: it
// works with both Qt's display and standalone Wayland clients.
struct vela_capture_job *vela_capture_begin(
    struct wl_shm *shm,
    struct ext_image_copy_capture_manager_v1 *manager,
    struct ext_image_capture_source_v1 *source,
    vela_capture_done_fn done,
    void *userdata);

const struct vela_capture_frame *vela_capture_pixels(const struct vela_capture_job *job);
void vela_capture_end(struct vela_capture_job *job);
bool vela_capture_supported_format(uint32_t format);
