// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BT_DESCRIBE_H
#define VELA_BT_DESCRIBE_H

// What the clients get to show a device (docs/bluetooth.md §6.2), and the
// names they may give it. Plain C, unit-tested.

#include <stdbool.h>
#include <stdint.h>

// The device's type, from BlueZ's Icon (itself made from the class) or,
// without it, from the Class of Device: mouse, keyboard, gamepad, tablet,
// headset, headphones, speaker, phone, computer, or other.
const char *vela_bt_device_type(const char *icon, uint32_t class_of_device);

// A name for Rename: UTF-8, no control characters, at most 248 bytes (the
// longest Bluetooth name). Empty gives the device its own name back.
bool vela_bt_valid_name(const char *name);

#endif
