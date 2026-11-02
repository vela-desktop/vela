// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Link-only boundaries for the unmodified production input parser. No root,
// /dev/uinput or real event injection: capture its libevdev output instead.
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <stdio.h>
#include <sys/types.h>

uid_t __wrap_getuid(void) { return 0; }
uid_t __wrap_geteuid(void) { return 0; }

int __wrap_libevdev_uinput_create_from_device(const struct libevdev *device, int fd,
                                             struct libevdev_uinput **input)
{
    if (libevdev_has_event_type(device, EV_ABS) &&
        !libevdev_has_event_code(device, EV_KEY, KEY_PAGEUP)) return -1;
    *input = (struct libevdev_uinput *)device;
    return 0;
}

int __wrap_libevdev_uinput_write_event(const struct libevdev_uinput *input,
                                      unsigned type, unsigned code, int value)
{
    fprintf(stderr, "EVENT %u %u %d\n", type, code, value);
    return 0;
}

const char *__wrap_libevdev_uinput_get_syspath(const struct libevdev_uinput *input)
{
    return "/sys/devices/virtual/input/input999999999";
}

void __wrap_libevdev_uinput_destroy(struct libevdev_uinput *input) { }
