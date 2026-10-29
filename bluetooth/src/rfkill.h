// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BT_RFKILL_H
#define VELA_BT_RFKILL_H

// rfkill switches (docs/bluetooth.md §6.3). The kernel registers one switch
// per Bluetooth adapter, under the adapter: <sysfs>/class/bluetooth/hciN/
// rfkillM, with its number in `index`. Some laptops also have platform
// switches of type Bluetooth with no adapter behind them (asus-wmi,
// hp-wmi). Vela only ever removes soft blocks, and only of the switches
// named here. `sysfs` and `device` are /sys and /dev/rfkill, or the tests'
// fakes.

#include <stdbool.h>

struct vela_bt_switch {
    int index;
    bool soft;
    bool hard;
};

// "hci0" for BlueZ's /org/bluez/hci0, or NULL if `path` isn't an adapter.
// Points into `path`.
const char *vela_bt_hci_name(const char *path);

// The switch of adapter `hci`. 0, or a negative errno (-ENOENT: none).
int vela_bt_adapter_switch(const char *sysfs, const char *hci, struct vela_bt_switch *out);

// The switches of type Bluetooth that belong to no adapter, at most `max`
// of them. Their number.
int vela_bt_platform_switches(const char *sysfs, struct vela_bt_switch *out, int max);

// Removes the soft block of switch `index` (RFKILL_OP_CHANGE: never
// CHANGE_ALL). 0, or a negative errno.
int vela_bt_unblock(const char *device, int index);

#endif
