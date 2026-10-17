// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_HELPER_H
#define VELA_BRIGHTNESS_HELPER_H

// One output's helper (docs/brightness.md §5): the scheduler, the
// transactions on its transport, and the lines. main.c gives it the socket
// and the real clock; the tests a fake monitor and a fake clock.

#include "lines.h"
#include "scheduler.h"
#include "transfer.h"

struct vela_helper {
    struct vela_scheduler scheduler;
    struct vela_transport transport;
    struct vela_clock *clock;
    // A line for the compositor, with its newline.
    void (*send)(void *data, const char *line);
    void *send_data;
    // `power on` from the compositor (the fake monitors wake up then).
    void (*power_on)(void *data);
    void *power_data;
    struct vela_line_reader reader;
};

void vela_helper_init(struct vela_helper *helper, struct vela_transport transport, struct vela_clock *clock,
    void (*send)(void *data, const char *line), void *data);

// What arrived from the compositor; whole lines are applied at once.
void vela_helper_feed(struct vela_helper *helper, const char *bytes, size_t length);
void vela_helper_line(struct vela_helper *helper, const char *line);

// Does at most one transaction, if one is due. Returns when to call again:
// now or earlier means at once (lines are read first), VELA_NEVER means
// when a line comes.
int64_t vela_helper_run(struct vela_helper *helper);

#endif
