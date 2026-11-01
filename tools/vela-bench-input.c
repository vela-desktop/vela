// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Persistent, root-controlled uinput instrument. The private stdin pipe
// accepts fixed F24/desktop actions and quit; no paths or shell commands.
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned long long now(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) exit(1);
    return (unsigned long long)time.tv_sec * 1000000000ull + time.tv_nsec;
}
static int key(struct libevdev_uinput *input, unsigned code, int value)
{
    return libevdev_uinput_write_event(input, EV_KEY, code, value) |
           libevdev_uinput_write_event(input, EV_SYN, SYN_REPORT, 0);
}
static int pulse(struct libevdev_uinput *input, unsigned code)
{
    int result = key(input, code, 1);
    return result | key(input, code, 0);
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: vela-bench-input [--desktop]\nPrivate stdin: pulse or quit for F24 calibration; desktop mode adds fixed shortcuts, search and bounded absolute points. No shell commands.");
        return 0;
    }
    int desktop = argc == 2 && !strcmp(argv[1], "--desktop");
    if ((argc != 1 && !desktop) || getuid() || geteuid()) {
        fputs("vela-bench-input: explicit root instrument invocation required\n", stderr);
        return 1;
    }
    struct libevdev *device = libevdev_new();
    struct libevdev_uinput *input = NULL;
    if (!device) return 1;
    libevdev_set_name(device, desktop ? "Vela benchmark desktop input" : "Vela benchmark calibration keyboard");
    libevdev_set_id_bustype(device, BUS_VIRTUAL);
    int setup = libevdev_enable_event_type(device, EV_KEY);
    // Advertise a normal keyboard so udev/libinput classifies it correctly;
    // Calibration emits only F24; desktop mode adds fixed shortcut actions.
    for (int key = KEY_ESC; key <= KEY_SPACE; ++key) setup |= libevdev_enable_event_code(device, EV_KEY, key, NULL);
    if (desktop) {
        setup |= libevdev_enable_event_code(device, EV_KEY, KEY_LEFTMETA, NULL);
        setup |= libevdev_enable_event_code(device, EV_KEY, KEY_LEFTALT, NULL);
        setup |= libevdev_enable_event_code(device, EV_KEY, KEY_F4, NULL);
        setup |= libevdev_enable_event_code(device, EV_KEY, KEY_LEFT, NULL);
        setup |= libevdev_enable_event_code(device, EV_KEY, KEY_RIGHT, NULL);
        setup |= libevdev_enable_event_code(device, EV_KEY, KEY_UP, NULL);
        setup |= libevdev_enable_event_code(device, EV_KEY, KEY_DOWN, NULL);
        setup |= libevdev_enable_event_code(device, EV_KEY, BTN_LEFT, NULL);
        setup |= libevdev_enable_event_code(device, EV_KEY, BTN_RIGHT, NULL);
        setup |= libevdev_enable_event_type(device, EV_ABS);
        const struct input_absinfo axis = { .minimum = 0, .maximum = 65535, .resolution = 100 };
        setup |= libevdev_enable_event_code(device, EV_ABS, ABS_X, &axis);
        setup |= libevdev_enable_event_code(device, EV_ABS, ABS_Y, &axis);
        // Dedicated absolute pointer: no relative axes and no touchpad/tool
        // capabilities. Coordinates cover the controlled primary-only output.
        setup |= libevdev_enable_property(device, INPUT_PROP_DIRECT);
    }
    if (setup || libevdev_enable_event_code(device, EV_KEY, KEY_F24, NULL) ||
        libevdev_uinput_create_from_device(device, LIBEVDEV_UINPUT_OPEN_MANAGED, &input)) {
        fputs("vela-bench-input: cannot create uinput instrument\n", stderr);
        libevdev_free(device);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *syspath = libevdev_uinput_get_syspath(input);
    if (!syspath) { libevdev_uinput_destroy(input); libevdev_free(device); return 1; }
    printf("{\"event\":\"uinput-ready\",\"method\":\"%s\",\"clock\":\"CLOCK_MONOTONIC\",\"sysfs\":\"%s\"}\n",
           desktop ? "linux-uinput-fixed-desktop-v1" : "linux-uinput-f24-v1", syspath);
    char command[80];
    unsigned count = 0;
    int failed = 0;
    while (fgets(command, sizeof(command), stdin)) {
        if (!strcmp(command, "quit\n")) break;
        if (++count > 1000) { failed = 1; break; }
        unsigned long long before = now();
        int result = 0;
        if (!strcmp(command, "pulse\n")) result = pulse(input, KEY_F24);
        else if (desktop && !strcmp(command, "launcher\n")) result = pulse(input, KEY_LEFTMETA);
        else if (desktop && !strcmp(command, "escape\n")) result = pulse(input, KEY_ESC);
        else if (desktop && !strcmp(command, "enter\n")) result = pulse(input, KEY_ENTER);
        else if (desktop && !strcmp(command, "down\n")) result = key(input, BTN_LEFT, 1);
        else if (desktop && !strcmp(command, "up\n")) result = key(input, BTN_LEFT, 0);
        else if (desktop && (!strcmp(command, "snap-left\n") || !strcmp(command, "snap-right\n")
                            || !strcmp(command, "maximize\n") || !strcmp(command, "restore\n"))) {
            unsigned code = !strcmp(command,"snap-left\n") ? KEY_LEFT : !strcmp(command,"snap-right\n") ? KEY_RIGHT
                : !strcmp(command,"maximize\n") ? KEY_UP : KEY_DOWN;
            result = key(input, KEY_LEFTMETA, 1);
            result |= pulse(input, code);
            result |= key(input, KEY_LEFTMETA, 0);
        }
        else if (desktop && !strcmp(command, "desktop\n")) {
            result = key(input, KEY_LEFTMETA, 1);
            result |= pulse(input, KEY_D);
            result |= key(input, KEY_LEFTMETA, 0);
        } else if (desktop && !strcmp(command, "close\n")) {
            result = key(input, KEY_LEFTALT, 1);
            result |= pulse(input, KEY_F4);
            result |= key(input, KEY_LEFTALT, 0);
        } else if (desktop && !strcmp(command, "search\n")) {
            // Unique fixture search token, fixed US physical keys. Sessions
            // with another effective layout must fail launch verification.
            const unsigned codes[] = {KEY_V,KEY_E,KEY_L,KEY_A,KEY_B,KEY_E,KEY_N,KEY_C,KEY_H,KEY_S,KEY_C,KEY_E,KEY_N,KEY_E};
            for (unsigned i = 0; i < sizeof(codes)/sizeof(codes[0]); ++i) result |= pulse(input, codes[i]);
        } else if (desktop) {
            unsigned x, y; char extra;
            if (sscanf(command, "point %u %u %c", &x, &y, &extra) != 2 || x > 65535 || y > 65535) { failed = 1; break; }
            result = libevdev_uinput_write_event(input, EV_ABS, ABS_X, x);
            result |= libevdev_uinput_write_event(input, EV_ABS, ABS_Y, y);
            result |= libevdev_uinput_write_event(input, EV_SYN, SYN_REPORT, 0);
        } else { failed = 1; break; }
        unsigned long long after = now();
        if (result) { failed = 1; break; }
        printf("{\"event\":\"injected\",\"action_id\":%u,\"before_monotonic_ns\":%llu,\"after_monotonic_ns\":%llu}\n", count, before, after);
    }
    if (desktop) {
        key(input, BTN_LEFT, 0); key(input, KEY_LEFTMETA, 0); key(input, KEY_LEFTALT, 0);
    }
    libevdev_uinput_destroy(input);
    libevdev_free(device);
    return failed;
}
