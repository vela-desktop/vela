// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Allocates a few GBM buffers on a render node and exports them as dmabufs,
// the way Vela's renderer and vela-slowgpu need them. Exit 0 only if all do.
#include <fcntl.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define MOD_LINEAR 0ull

static int try_bo(const char *what, struct gbm_bo *bo)
{
    if (!bo) {
        printf("  %-34s FAIL\n", what);
        return 0;
    }
    int fd = gbm_bo_get_fd(bo);
    printf("  %-34s ok (modifier 0x%llx, dmabuf %s)\n", what,
           (unsigned long long)gbm_bo_get_modifier(bo), fd >= 0 ? "ok" : "FAIL");
    if (fd >= 0)
        close(fd);
    gbm_bo_destroy(bo);
    return fd >= 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s /dev/dri/renderD128\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    struct gbm_device *dev = gbm_create_device(fd);
    if (!dev) {
        puts("gbm_create_device: FAIL");
        return 1;
    }
    printf("GBM backend: %s\n", gbm_device_get_backend_name(dev));

    const uint64_t linear = MOD_LINEAR;
    int ok = 1;
    ok &= try_bo("XRGB8888 rendering",
                 gbm_bo_create(dev, 256, 256, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING));
    ok &= try_bo("ARGB8888 rendering",
                 gbm_bo_create(dev, 256, 256, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING));
    ok &= try_bo("XRGB8888 linear modifier",
                 gbm_bo_create_with_modifiers(dev, 256, 256, GBM_FORMAT_XRGB8888, &linear, 1));
    // As vela-slowgpu allocates its buffers.
    ok &= try_bo("XRGB8888 GBM_BO_USE_LINEAR",
                 gbm_bo_create(dev, 256, 256, GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR));

    gbm_device_destroy(dev);
    close(fd);
    puts(ok ? "GBM: PASS" : "GBM: FAIL");
    return ok ? 0 : 1;
}
