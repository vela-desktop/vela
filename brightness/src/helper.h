// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_HELPER_H
#define VELA_BRIGHTNESS_HELPER_H

// One output's helper (docs/brightness.md §5): the scheduler, the
// transactions on its backend, and the lines. main.c gives it the socket
// and the real clock; the tests a fake monitor and a fake clock.

#include "lines.h"
#include "scheduler.h"
#include "transfer.h"

// Where the brightness is: DDC/CI on a transport, or a backlight
// (backlight.h). One call is one transaction.
struct vela_backend {
    enum vela_result (*read)(void *data, uint32_t *value, uint32_t *max);
    // `applied`: what the device was given (the value, unless the backend
    // keeps it in a range of its own).
    enum vela_result (*write)(void *data, uint32_t value, uint32_t *applied);
    void *data;
};

struct vela_helper {
    struct vela_scheduler scheduler;
    struct vela_backend backend;
    struct vela_transport transport; // DDC/CI
    struct vela_clock *clock;
    // A line for the compositor, with its newline.
    void (*send)(void *data, const char *line);
    void *send_data;
    // `power on` from the compositor (the fake monitors wake up then).
    void (*power_on)(void *data);
    void *power_data;
    struct vela_line_reader reader;
};

// DDC/CI on `transport`.
void vela_helper_init(struct vela_helper *helper, struct vela_transport transport, struct vela_clock *clock,
    void (*send)(void *data, const char *line), void *data);
// Any backend, with the scheduler's timing for it.
void vela_helper_init_backend(struct vela_helper *helper, struct vela_backend backend, int64_t write_interval,
    int64_t readback_delay, struct vela_clock *clock, void (*send)(void *data, const char *line), void *data);

// What arrived from the compositor; whole lines are applied at once.
void vela_helper_feed(struct vela_helper *helper, const char *bytes, size_t length);
void vela_helper_line(struct vela_helper *helper, const char *line);

// Does at most one transaction, if one is due. Returns when to call again:
// now or earlier means at once (lines are read first), VELA_NEVER means
// when a line comes.
int64_t vela_helper_run(struct vela_helper *helper);

#endif
