// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "device.h"

#include <errno.h>
#include <string.h>

int vela_device_open(struct vela_device *device, const char *connector, const char *sysfs,
    const char *fake_path, struct vela_clock *clock, char *error, size_t size)
{
    memset(device, 0, sizeof(*device));
    if (fake_path && *fake_path) {
        device->is_fake = true;
        int found = vela_fake_load(&device->fake, fake_path, connector, error, size);
        if (found == -ENOENT) {
            device->fake.no_bus = true; // no section: as if the connector had no bus
        } else if (found < 0) {
            return found;
        }
        device->has_bus = !device->fake.no_bus;
        vela_fake_start(&device->fake, clock);
        device->transport = vela_fake_transport(&device->fake);
        return 0;
    }
    if (vela_i2c_find_bus(sysfs, connector, device->bus, sizeof(device->bus), &device->how) == 0) {
        device->has_bus = true;
        vela_i2c_init(&device->i2c, device->bus);
        device->transport = vela_i2c_transport(&device->i2c);
        return 0;
    }
    vela_fake_defaults(&device->fake);
    device->fake.no_bus = true;
    vela_fake_start(&device->fake, clock);
    device->transport = vela_fake_transport(&device->fake);
    return 0;
}
