// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helper.h"

#include <stdio.h>
#include <string.h>

static void send_event(void *data, const struct vela_event *event)
{
    struct vela_helper *helper = data;
    char line[VELA_LINE_MAX];
    vela_format_event(line, sizeof(line), event);
    helper->send(helper->send_data, line);
}

static int64_t monotonic(struct vela_helper *helper)
{
    return helper->clock->monotonic(helper->clock->data);
}

// Both clocks, so a suspend is noticed before anything else is decided.
static int64_t tick(struct vela_helper *helper)
{
    int64_t now = monotonic(helper);
    vela_scheduler_clock(&helper->scheduler, now, helper->clock->boottime(helper->clock->data));
    return now;
}

void vela_helper_init(struct vela_helper *helper, struct vela_transport transport, struct vela_clock *clock,
    void (*send)(void *data, const char *line), void *data)
{
    memset(helper, 0, sizeof(*helper));
    helper->transport = transport;
    helper->clock = clock;
    helper->send = send;
    helper->send_data = data;
    vela_scheduler_init(&helper->scheduler, monotonic(helper), clock->boottime(clock->data), send_event, helper);
}

void vela_helper_line(struct vela_helper *helper, const char *line)
{
    struct vela_command command = vela_parse_command(line);
    int64_t now = tick(helper);
    switch (command.kind) {
    case VELA_COMMAND_SET:
        vela_scheduler_set(&helper->scheduler, command.seq, command.value, now);
        break;
    case VELA_COMMAND_LIVE:
        vela_scheduler_live(&helper->scheduler, command.on);
        break;
    case VELA_COMMAND_POWER:
        if (command.on && !helper->scheduler.power && helper->power_on) {
            helper->power_on(helper->power_data);
        }
        vela_scheduler_power(&helper->scheduler, command.on, now);
        break;
    case VELA_COMMAND_SESSION:
        vela_scheduler_session(&helper->scheduler, command.on, now);
        break;
    case VELA_COMMAND_PROBE:
        vela_scheduler_probe(&helper->scheduler, now);
        break;
    case VELA_COMMAND_INVALID:
        fprintf(stderr, "vela-brightness: unknown line \"%s\"\n", line);
        break;
    }
}

static void feed_line(void *data, const char *line)
{
    vela_helper_line(data, line);
}

void vela_helper_feed(struct vela_helper *helper, const char *bytes, size_t length)
{
    vela_line_reader_feed(&helper->reader, bytes, length, feed_line, helper);
}

int64_t vela_helper_run(struct vela_helper *helper)
{
    int64_t now = tick(helper);
    struct vela_action action = vela_scheduler_next(&helper->scheduler, now);
    if (action.kind == VELA_ACTION_WAIT) {
        return action.at;
    }
    // The compositor's watchdog measures from `busy` (§6).
    helper->send(helper->send_data, "busy\n");
    if (action.kind == VELA_ACTION_READ) {
        uint16_t value = 0;
        uint16_t max = 0;
        enum vela_result result = vela_ddc_read(&helper->transport, helper->clock, &value, &max, NULL);
        helper->send(helper->send_data, "idle\n");
        vela_scheduler_read_done(&helper->scheduler, result, value, max, tick(helper));
    } else {
        enum vela_result result = vela_ddc_write(&helper->transport, helper->clock, action.value, NULL);
        helper->send(helper->send_data, "idle\n");
        vela_scheduler_write_done(&helper->scheduler, result, tick(helper));
    }
    return monotonic(helper);
}
