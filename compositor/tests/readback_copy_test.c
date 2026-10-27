// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/readback_copy.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void exercise(size_t width, size_t rows, size_t extra, bool opaque, unsigned workers)
{
    const size_t row_bytes = width * 4;
    const size_t src_stride = row_bytes + extra;
    const size_t dst_stride = row_bytes + extra + 13;
    const size_t src_size = src_stride * rows + 16;
    const size_t dst_size = dst_stride * rows + 16;
    uint8_t *src = malloc(src_size), *want = malloc(dst_size), *got = malloc(dst_size);
    assert(src && want && got);
    for (size_t i = 0; i < src_size; ++i) {
        src[i] = (uint8_t)(i * 131u + 47u);
    }
    memset(want, 0xc3, dst_size);
    memset(got, 0xc3, dst_size);
    vela_readback_copy(want, dst_stride, src, src_stride, row_bytes, rows, opaque, 1);
    vela_readback_copy(got, dst_stride, src, src_stride, row_bytes, rows, opaque, workers);
    assert(memcmp(want, got, dst_size) == 0);
    for (size_t y = 0; y < rows; ++y) {
        const uint8_t *row = got + y * dst_stride;
        for (size_t x = row_bytes; x < dst_stride; ++x) {
            assert(row[x] == 0xc3);
        }
        if (opaque) {
            for (size_t x = 3; x < row_bytes; x += 4) {
                assert(row[x] == 0xff);
            }
        }
    }
    for (size_t i = dst_stride * rows; i < dst_size; ++i) {
        assert(got[i] == 0xc3);
    }
    free(src);
    free(want);
    free(got);
}

int main(void)
{
    for (unsigned count = 1; count <= 8; ++count) {
        exercise(1, 1, 0, false, count);
        exercise(17, 7, 16, true, count);
        exercise(1024, 512, 0, false, count);
        exercise(1024, 512, 32, true, count);
        exercise(2560, 1440, 0, false, count);
    }
    puts("readback-copy: all contiguous, padded, alpha and worker-count cases passed");
    return 0;
}
