// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-brightness: the brightness of one output (docs/brightness.md §5).
//
//   vela-brightness ddc <connector>      the helper, started by the compositor
//   vela-brightness backlight <device>   the same for a built-in screen
//   vela-brightness --probe [connector]  what each monitor answers (read only)
//   vela-brightness --bench <connector>  measures the timings (writes)
//
// The helper talks on the socket in VELA_BRIGHTNESS_FD, or on stdin and
// stdout without it (to try it by hand). It exits when the other side
// closes.
//
// With VELA_BRIGHTNESS_FAKE (fake monitors), VELA_BRIGHTNESS_SYSFS names a
// fake sysfs tree for the backlights, written straight into its files: the
// tests never reach logind. Without fake monitors it is ignored. Both exist
// only in builds with the tests (VELA_BUILD_TOOLS, VELA_BRIGHTNESS_TESTING):
// an installed helper never opens a file its environment names. They are
// read with secure_getenv, so even a test build given privileges (setuid,
// file capabilities) can't be pointed at other files by its caller.

#include "backlight.h"
#include "device.h"
#include "helper.h"
#include "probe.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SYSFS "/sys"

static int out_fd = 1;

static void send_line(void *data, const char *line)
{
    size_t length = strlen(line);
    while (length > 0) {
        ssize_t done = write(out_fd, line, length);
        if (done < 0 && errno == EINTR) {
            continue;
        }
        if (done <= 0) {
            exit(0); // the compositor is gone
        }
        line += done;
        length -= (size_t)done;
    }
}

static void fake_power_on(void *data)
{
    vela_fake_power_on(data);
}

static int usage(void)
{
    fprintf(stderr,
        "usage: vela-brightness ddc <connector>\n"
        "       vela-brightness backlight <device>\n"
        "       vela-brightness --probe [connector]\n"
        "       vela-brightness --bench <connector>\n");
    return 2;
}

// The tests' fake monitors, or NULL.
static const char *fake_monitors(void)
{
#ifdef VELA_BRIGHTNESS_TESTING
    const char *fake = secure_getenv("VELA_BRIGHTNESS_FAKE");
    return fake && *fake ? fake : NULL;
#else
    return NULL;
#endif
}

// The tests' fake sysfs tree, or NULL.
static const char *fake_tree(void)
{
#ifdef VELA_BRIGHTNESS_TESTING
    const char *tree = secure_getenv("VELA_BRIGHTNESS_SYSFS");
    return fake_monitors() && tree && *tree ? tree : NULL;
#else
    return NULL;
#endif
}

static int run_helper(bool backlight_mode, const char *name)
{
    int in_fd = 0;
    const char *fd_text = getenv("VELA_BRIGHTNESS_FD");
    if (fd_text && *fd_text) {
        char *end;
        long fd = strtol(fd_text, &end, 10);
        if (*end != '\0' || fd < 0 || fd > INT_MAX) {
            fprintf(stderr, "vela-brightness: bad VELA_BRIGHTNESS_FD \"%s\"\n", fd_text);
            return 2;
        }
        in_fd = out_fd = (int)fd;
    }
    signal(SIGPIPE, SIG_IGN);

    static struct vela_helper helper;
    if (backlight_mode) {
        static struct vela_backlight backlight;
        const char *tree = fake_tree();
        if (vela_backlight_init(&backlight, tree ? tree : SYSFS, name, tree != NULL, &vela_system_clock) < 0) {
            fprintf(stderr, "vela-brightness: bad backlight \"%s\"\n", name);
            return 2;
        }
        vela_helper_init_backend(&helper, vela_backlight_backend(&backlight), VELA_BACKLIGHT_WRITE_INTERVAL_MS,
            VELA_BACKLIGHT_READBACK_MS, &vela_system_clock, send_line, NULL);
    } else {
        static struct vela_device device;
        char error[256];
        if (vela_device_open(&device, name, SYSFS, fake_monitors(), &vela_system_clock, error,
                sizeof(error)) < 0) {
            fprintf(stderr, "vela-brightness: %s\n", error);
            return 1;
        }
        vela_helper_init(&helper, device.transport, &vela_system_clock, send_line, NULL);
        if (device.is_fake) {
            helper.power_on = fake_power_on;
            helper.power_data = &device.fake;
        }
    }

    for (;;) {
        int64_t at = vela_helper_run(&helper);
        int64_t now = vela_system_clock.monotonic(NULL);
        int timeout = at == VELA_NEVER ? -1 : at <= now ? 0 : at - now > INT_MAX ? INT_MAX : (int)(at - now);
        struct pollfd poll_fd = { .fd = in_fd, .events = POLLIN };
        int ready = poll(&poll_fd, 1, timeout);
        if (ready < 0 && errno != EINTR) {
            perror("vela-brightness: poll");
            return 1;
        }
        if (ready > 0) {
            char bytes[512];
            ssize_t length = read(in_fd, bytes, sizeof(bytes));
            if (length < 0 && errno == EINTR) {
                continue;
            }
            if (length <= 0) {
                return 0; // the compositor closed its side
            }
            vela_helper_feed(&helper, bytes, (size_t)length);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && (strcmp(argv[1], "ddc") == 0 || strcmp(argv[1], "backlight") == 0)) {
        return run_helper(strcmp(argv[1], "backlight") == 0, argv[2]);
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "--probe") == 0) {
        return vela_probe(stdout, SYSFS, fake_monitors(), argc == 3 ? argv[2] : NULL);
    }
    if (argc == 3 && strcmp(argv[1], "--bench") == 0) {
        static struct vela_device device;
        char error[256];
        if (vela_device_open(&device, argv[2], SYSFS, fake_monitors(), &vela_system_clock, error,
                sizeof(error)) < 0) {
            fprintf(stderr, "vela-brightness: %s\n", error);
            return 1;
        }
        setvbuf(stdout, NULL, _IOLBF, 0);
        return vela_bench(stdout, &device.transport, &vela_system_clock);
    }
    return usage();
}
