// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_BACKLIGHT_H
#define VELA_BRIGHTNESS_BACKLIGHT_H

// A built-in screen's backlight (docs/brightness.md §5.3): read in sysfs
// (readable by anyone), written through logind's SetBrightness for the
// session, so no root is needed.
//
// - Never 0 when the device has more than 20 levels: 0 can turn the panel
//   off; on coarse devices 0 is a level.
// - The OLED guard: after a write at or below 5% of the maximum, an
//   actual_brightness of 0 raises the value by one, up to three times.
//
// On a fake sysfs tree (the tests) the value is written straight into its
// `brightness` file: nothing reaches logind or the hardware.

#include "helper.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#define VELA_BACKLIGHT_COARSE 20 // levels up to which 0 is allowed
#define VELA_BACKLIGHT_OLED_RAISES 3

struct vela_backlight {
    char dir[PATH_MAX]; // <sysfs>/class/backlight/<device>
    char device[NAME_MAX + 1];
    struct vela_clock *clock;
    // Sets `value`: 0 or a negative errno (-EACCES: not allowed).
    int (*set)(void *data, const char *device, const char *dir, uint32_t value);
    void *set_data;
};

// Fills `backlight` for `device` under `sysfs`. -EINVAL for a name that
// isn't a plain file name. `set` is logind's unless `fake_tree`.
int vela_backlight_init(struct vela_backlight *backlight, const char *sysfs, const char *device, bool fake_tree,
    struct vela_clock *clock);

enum vela_result vela_backlight_read(struct vela_backlight *backlight, uint32_t *value, uint32_t *max);
enum vela_result vela_backlight_write(struct vela_backlight *backlight, uint32_t value, uint32_t *applied);

struct vela_backend vela_backlight_backend(struct vela_backlight *backlight);

// The two ways to set it.
int vela_backlight_set_logind(void *data, const char *device, const char *dir, uint32_t value);
int vela_backlight_set_file(void *data, const char *device, const char *dir, uint32_t value);

#endif
