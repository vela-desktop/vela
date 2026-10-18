// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "helper.h"

#include <stdint.h>
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

void vela_helper_init_backend(struct vela_helper *helper, struct vela_backend backend, int64_t write_interval,
    int64_t readback_delay, struct vela_clock *clock, void (*send)(void *data, const char *line), void *data)
{
    memset(helper, 0, sizeof(*helper));
    helper->backend = backend;
    helper->clock = clock;
    helper->send = send;
    helper->send_data = data;
    vela_scheduler_init(&helper->scheduler, monotonic(helper), clock->boottime(clock->data), send_event, helper);
    helper->scheduler.write_interval = write_interval;
    helper->scheduler.readback_delay = readback_delay;
}

static enum vela_result ddc_read(void *data, uint32_t *value, uint32_t *max)
{
    struct vela_helper *helper = data;
    uint16_t v = 0, m = 0;
    enum vela_result result = vela_ddc_read(&helper->transport, helper->clock, &v, &m, NULL);
    *value = v;
    *max = m;
    return result;
}

static enum vela_result ddc_write(void *data, uint32_t value, uint32_t *applied)
{
    struct vela_helper *helper = data;
    *applied = value;
    return vela_ddc_write(&helper->transport, helper->clock, value > UINT16_MAX ? UINT16_MAX : (uint16_t)value, NULL);
}

void vela_helper_init(struct vela_helper *helper, struct vela_transport transport, struct vela_clock *clock,
    void (*send)(void *data, const char *line), void *data)
{
    struct vela_backend backend = { .read = ddc_read, .write = ddc_write, .data = helper };
    vela_helper_init_backend(helper, backend, VELA_WRITE_INTERVAL_MS, VELA_READBACK_MS, clock, send, data);
    helper->transport = transport;
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
    struct vela_backend *backend = &helper->backend;
    if (action.kind == VELA_ACTION_READ) {
        uint32_t value = 0;
        uint32_t max = 0;
        enum vela_result result = backend->read(backend->data, &value, &max);
        helper->send(helper->send_data, "idle\n");
        vela_scheduler_read_done(&helper->scheduler, result, value, max, tick(helper));
    } else {
        uint32_t applied = action.value;
        enum vela_result result = backend->write(backend->data, action.value, &applied);
        helper->send(helper->send_data, "idle\n");
        vela_scheduler_write_done(&helper->scheduler, result, applied, tick(helper));
    }
    return monotonic(helper);
}
