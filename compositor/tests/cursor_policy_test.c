// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/cursor_policy.h"
#include <stdio.h>

// Always-on assertions: release CI builds often define NDEBUG, which would
// otherwise skip every check in this test (and trigger unused-variable errors).
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "cursor policy failed: %s at line %d\\n", #expr, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void)
{
    const uint64_t linear[] = { DRM_FORMAT_MOD_LINEAR };
    const uint64_t tiled[] = { 0x100000000000001ULL };
    const uint64_t many[] = { DRM_FORMAT_MOD_LINEAR, 0x100000000000001ULL };

    // Strict cursor requests, never a size-only classifier.
    CHECK(vela_cursor_buffer_format(DRM_FORMAT_ARGB8888, linear, 1, 64, 64));
    CHECK(vela_cursor_buffer_format(DRM_FORMAT_ARGB8888, linear, 1, 256, 256));
    CHECK(!vela_cursor_buffer_format(DRM_FORMAT_XRGB8888, linear, 1, 64, 64));
    CHECK(!vela_cursor_buffer_format(DRM_FORMAT_ARGB8888, tiled, 1, 64, 64));
    CHECK(!vela_cursor_buffer_format(DRM_FORMAT_ARGB8888, many, 2, 64, 64));
    CHECK(!vela_cursor_buffer_format(DRM_FORMAT_ARGB8888, linear, 1, 257, 64));
    CHECK(!vela_cursor_buffer_format(DRM_FORMAT_ARGB8888, linear, 1, 0, 64));
    CHECK(!vela_cursor_buffer_format(DRM_FORMAT_ARGB8888, NULL, 1, 64, 64));

    // Non-DRM, unsupported formats/sizes or variable hardware cursor
    // sizes must all leave wlroots' normal allocator path untouched.
    CHECK(vela_cursor_swapchain_eligible(true, true, 1, 256, 256, true, true));
    CHECK(!vela_cursor_swapchain_eligible(false, true, 1, 256, 256, true, true));
    CHECK(!vela_cursor_swapchain_eligible(true, false, 1, 256, 256, true, true));
    CHECK(!vela_cursor_swapchain_eligible(true, true, 2, 256, 256, true, true));
    CHECK(!vela_cursor_swapchain_eligible(true, true, 1, 512, 512, true, true));
    CHECK(!vela_cursor_swapchain_eligible(true, true, 1, 256, 256, false, true));
    CHECK(!vela_cursor_swapchain_eligible(true, true, 1, 256, 256, true, false));

    // An ordinary 128x128 LINEAR capture uses NORMAL allocation, not
    // CURSOR|WRITE. Cursor allocation never falls back to RENDERING-only:
    // the cursor-plane candidate must pass the Vulkan probe and DRM commit.
    CHECK(vela_gbm_buffer_flags(false, false) == (GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING));
    CHECK(vela_gbm_buffer_flags(false, true) == GBM_BO_USE_RENDERING);
    CHECK(vela_gbm_buffer_flags(true, false) == (GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE));
    CHECK(vela_gbm_buffer_flags(true, true) == 0);
    puts("cursor allocation policy: OK");
    return 0;
}
