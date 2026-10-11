// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wl_shm;
struct ext_image_capture_source_v1;
struct ext_image_copy_capture_manager_v1;
struct vela_capture_job;

/** A captured SHM frame with immutable allocated dimensions and stride.
 * Pixels and metadata are borrowed from the job and become invalid when
 * vela_capture_end() destroys the job; callers must not retain them.
 */
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

/** Begin a one-shot SHM capture using the caller-owned Wayland connection.
 * On success the job owns source and calls done once, even on capture failure.
 * The callback may call vela_capture_end(). On NULL, source remains caller-owned.
 * Dispatch the display from one thread only and cancel outstanding jobs before
 * destroying the connection. Qt and standalone Wayland clients are supported.
 */
struct vela_capture_job *vela_capture_begin(
    struct wl_shm *shm,
    struct ext_image_copy_capture_manager_v1 *manager,
    struct ext_image_capture_source_v1 *source,
    vela_capture_done_fn done,
    void *userdata);

/** Return borrowed pixels after a successful, validated frame, else NULL. */
const struct vela_capture_frame *vela_capture_pixels(const struct vela_capture_job *job);
/** Destroy protocol proxies and SHM resources; cancels pending work. */
void vela_capture_end(struct vela_capture_job *job);
/** Whether this 32-bit SHM pixel format can be decoded by the clients. */
bool vela_capture_supported_format(uint32_t format);
/** Pure, overflow-safe bounds check for all pixel rows within SHM length. */
bool vela_capture_frame_valid(const struct vela_capture_frame *frame);

#ifdef __cplusplus
}
#endif
