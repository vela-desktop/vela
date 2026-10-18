// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_I2C_H
#define VELA_BRIGHTNESS_I2C_H

// The real transport: a DRM connector's I²C bus, /dev/i2c-N
// (docs/brightness.md §5.1, §5.2).

#include "transfer.h"

#include <stdbool.h>
#include <stddef.h>

// The bus of a DRM connector ("card1-DP-3"), looked for under `sysfs`
// ("/sys", or a test's directory): the connector's own i2c-N directory
// (DisplayPort AUX) first, then its `ddc` link (HDMI, DVI, VGA). For
// DisplayPort the `ddc` link names a bus that doesn't reach the monitor.
// Writes the bus ("i2c-9") into `bus`; `how` (may be NULL) is "aux" or
// "ddc". Returns 0, -ENOENT without a bus, -EINVAL for a bad connector.
int vela_i2c_find_bus(const char *sysfs, const char *connector, char *bus, size_t size,
    const char **how);

struct vela_i2c {
    char path[64]; // /dev/i2c-N
    int fd; // only during a transaction
    // The driver refused I2C_RDWR (some NVIDIA builds): I2C_SLAVE and
    // read()/write(), for the rest of the process.
    bool plain;
    bool addressed; // I2C_SLAVE done on this fd
};

void vela_i2c_init(struct vela_i2c *i2c, const char *bus);
struct vela_transport vela_i2c_transport(struct vela_i2c *i2c);

#endif
