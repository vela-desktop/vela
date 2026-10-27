// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VELA_READBACK_COPY_H
#define VELA_READBACK_COPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Copy a mapped Vulkan readback staging allocation into a client-owned CPU
// buffer. Vulkan completion and host visibility MUST be established before
// calling this function, and the caller retains both buffers until return.
//
// workers=1 is the unchanged synchronous baseline. 2..4 workers are a
// strictly opt-in performance experiment, not an asynchronous protocol path.
void vela_readback_copy(uint8_t *dst, size_t dst_stride, const uint8_t *src,
    size_t src_stride, size_t row_bytes, size_t rows, bool force_opaque, unsigned workers);

#endif
