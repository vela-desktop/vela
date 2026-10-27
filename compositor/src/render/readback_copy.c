// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/readback_copy.h"

#include <pthread.h>
#include <string.h>

struct copy_job {
    uint8_t *dst;
    const uint8_t *src;
    size_t dst_stride, src_stride, row_bytes;
    size_t first, end;
    bool force_opaque;
};

static void copy_rows(const struct copy_job *job)
{
    // The staging copy is tightly packed; the destination may have padding.
    // Never overwrite padding or apply alpha fixup outside actual pixel data.
    for (size_t y = job->first; y < job->end; ++y) {
        uint8_t *dst = job->dst + y * job->dst_stride;
        memcpy(dst, job->src + y * job->src_stride, job->row_bytes);
        if (job->force_opaque) {
            for (size_t x = 3; x < job->row_bytes; x += 4) {
                dst[x] = 0xff;
            }
        }
    }
}

static void *copy_thread(void *context)
{
    copy_rows(context);
    return NULL;
}

void vela_readback_copy(uint8_t *dst, size_t dst_stride, const uint8_t *src,
    size_t src_stride, size_t row_bytes, size_t rows, bool force_opaque, unsigned workers)
{
    struct copy_job jobs[4] = { 0 };
    pthread_t threads[3];
    bool started[3] = { false };

    // Small copies must not pay thread creation overhead. Keep a bounded
    // number of workers and refuse to split fewer rows than there are jobs.
    unsigned count = workers >= 2 && rows >= 2 && row_bytes >= 1024
            && rows <= SIZE_MAX / row_bytes && row_bytes * rows >= 1024 * 1024
        ? (workers > 4 ? 4 : workers)
        : 1;
    if (count > rows) {
        count = (unsigned)rows;
    }

    for (unsigned i = 0; i < count; ++i) {
        jobs[i] = (struct copy_job) {
            .dst = dst, .src = src, .dst_stride = dst_stride,
            .src_stride = src_stride, .row_bytes = row_bytes,
            .first = rows * i / count, .end = rows * (i + 1) / count,
            .force_opaque = force_opaque,
        };
    }

    // Only CPU memcpy runs on these workers, never Vulkan or Wayland API
    // calls. Failed pthread_create uses a synchronous fallback for that
    // disjoint slice. All successful threads are joined before return.
    for (unsigned i = 0; i + 1 < count; ++i) {
        if (pthread_create(&threads[i], NULL, copy_thread, &jobs[i]) == 0) {
            started[i] = true;
        } else {
            copy_rows(&jobs[i]);
        }
    }
    copy_rows(&jobs[count - 1]);
    for (unsigned i = 0; i + 1 < count; ++i) {
        if (started[i]) {
            pthread_join(threads[i], NULL);
        }
    }
}
