// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "transfer.h"

#include <errno.h>
#include <string.h>
#include <time.h>

static int64_t clock_ms(clockid_t id)
{
    struct timespec now;
    clock_gettime(id, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int64_t system_monotonic(void *data)
{
    return clock_ms(CLOCK_MONOTONIC);
}

static int64_t system_boottime(void *data)
{
    return clock_ms(CLOCK_BOOTTIME);
}

static void system_sleep(void *data, int64_t ms)
{
    struct timespec wait = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
    while (nanosleep(&wait, &wait) < 0 && errno == EINTR) {
    }
}

struct vela_clock vela_system_clock = {
    .monotonic = system_monotonic,
    .boottime = system_boottime,
    .sleep = system_sleep,
};

const char *vela_result_name(enum vela_result result)
{
    switch (result) {
    case VELA_RESULT_OK:
        return "ok";
    case VELA_RESULT_UNSUPPORTED:
        return "unsupported";
    case VELA_RESULT_NO_ANSWER:
        return "no-answer";
    case VELA_RESULT_BUSY:
        return "busy";
    case VELA_RESULT_NO_ACCESS:
        return "no-access";
    case VELA_RESULT_NO_BUS:
        break;
    }
    return "no-bus";
}

static int64_t now(struct vela_clock *clock)
{
    return clock->monotonic(clock->data);
}

// Opens and locks the device. VELA_RESULT_OK: the caller closes it.
static enum vela_result begin(struct vela_transport *transport, struct vela_clock *clock,
    struct vela_transaction_log *log)
{
    int error = transport->ops->open(transport->data);
    if (log) {
        log->open_error = -error;
    }
    if (error == -ENOENT || error == -ENODEV || error == -ENXIO) {
        return VELA_RESULT_NO_BUS;
    }
    if (error < 0) {
        return VELA_RESULT_NO_ACCESS;
    }
    int64_t start = now(clock);
    while ((error = transport->ops->lock(transport->data)) == -EWOULDBLOCK) {
        if (now(clock) - start >= VELA_DDC_LOCK_BUDGET_MS) {
            break;
        }
        clock->sleep(clock->data, VELA_DDC_LOCK_RETRY_MS);
    }
    if (log) {
        log->lock_ms = now(clock) - start;
    }
    if (error < 0) {
        transport->ops->close(transport->data);
        return error == -EWOULDBLOCK ? VELA_RESULT_BUSY : VELA_RESULT_NO_ACCESS;
    }
    return VELA_RESULT_OK;
}

enum vela_result vela_ddc_read(struct vela_transport *transport, struct vela_clock *clock,
    uint16_t *value, uint16_t *max, struct vela_transaction_log *log)
{
    int64_t start = now(clock);
    if (log) {
        memset(log, 0, sizeof(*log));
    }
    enum vela_result result = begin(transport, clock, log);
    if (result != VELA_RESULT_OK) {
        if (log) {
            log->total_ms = now(clock) - start;
        }
        return result;
    }
    uint8_t request[VELA_DDC_GET_LENGTH];
    size_t length = vela_ddc_encode_get(request, VELA_DDC_BRIGHTNESS);
    result = VELA_RESULT_NO_ANSWER;
    for (int i = 0; i < VELA_DDC_READ_ATTEMPTS && result == VELA_RESULT_NO_ANSWER; ++i) {
        if (i > 0) {
            clock->sleep(clock->data, VELA_DDC_RETRY_MS);
        }
        struct vela_attempt attempt = { .reply = VELA_DDC_REPLY_BAD };
        int64_t sent = now(clock);
        int error = transport->ops->write(transport->data, request, length);
        if (error == 0) {
            clock->sleep(clock->data, VELA_DDC_REPLY_WAIT_MS);
            error = transport->ops->read(transport->data, attempt.bytes, sizeof(attempt.bytes));
        }
        if (error == 0) {
            attempt.length = sizeof(attempt.bytes);
            uint16_t got_value = 0;
            uint16_t got_max = 0;
            attempt.reply = vela_ddc_parse_reply(attempt.bytes, attempt.length, VELA_DDC_BRIGHTNESS,
                &got_value, &got_max);
            if (attempt.reply == VELA_DDC_REPLY_OK && got_max == 0) {
                // A maximum of 0 is no brightness to change (§5.5).
                result = VELA_RESULT_UNSUPPORTED;
            } else if (attempt.reply == VELA_DDC_REPLY_OK) {
                *value = got_value < got_max ? got_value : got_max;
                *max = got_max;
                result = VELA_RESULT_OK;
            } else if (attempt.reply == VELA_DDC_REPLY_UNSUPPORTED) {
                result = VELA_RESULT_UNSUPPORTED;
            }
        }
        attempt.error = -error;
        attempt.ms = now(clock) - sent;
        if (log) {
            log->attempt[log->attempts++] = attempt;
        }
    }
    transport->ops->close(transport->data);
    if (log) {
        log->total_ms = now(clock) - start;
    }
    return result;
}

enum vela_result vela_ddc_write(struct vela_transport *transport, struct vela_clock *clock,
    uint16_t value, struct vela_transaction_log *log)
{
    int64_t start = now(clock);
    if (log) {
        memset(log, 0, sizeof(*log));
    }
    enum vela_result result = begin(transport, clock, log);
    if (result == VELA_RESULT_OK) {
        uint8_t request[VELA_DDC_SET_LENGTH];
        size_t length = vela_ddc_encode_set(request, VELA_DDC_BRIGHTNESS, value);
        // Monitors don't answer a Set VCP: a write that went through may
        // still be dropped, and only a read back tells (§3, §5.4).
        int error = transport->ops->write(transport->data, request, length);
        if (log) {
            log->attempts = 1;
            log->attempt[0].error = -error;
        }
        result = error == 0 ? VELA_RESULT_OK : VELA_RESULT_NO_ANSWER;
        transport->ops->close(transport->data);
    }
    if (log) {
        log->total_ms = now(clock) - start;
    }
    return result;
}
