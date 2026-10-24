// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_DEVICE_H
#define VELA_BRIGHTNESS_DEVICE_H

// The transport of one connector: its I²C bus, or its fake monitor when
// VELA_BRIGHTNESS_FAKE names a file (docs/brightness.md §12). A connector
// without a bus, or without a section in the fake file, gets a transport
// that always answers "no bus".

#include "fake.h"
#include "i2c.h"

#include <stdbool.h>
#include <stddef.h>

struct vela_device {
    struct vela_transport transport;
    struct vela_i2c i2c;
    struct vela_fake_monitor fake;
    bool is_fake;
    bool has_bus;
    char bus[32]; // "i2c-9"
    const char *how; // "aux", "ddc"
};

// 0, or -errno with `error` filled when the fake file is unreadable or bad.
int vela_device_open(struct vela_device *device, const char *connector, const char *sysfs,
    const char *fake_path, struct vela_clock *clock, char *error, size_t size);

#endif
