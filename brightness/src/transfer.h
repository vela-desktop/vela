// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_TRANSFER_H
#define VELA_BRIGHTNESS_TRANSFER_H

// One DDC/CI transaction (docs/brightness.md §5.2): open the device, lock
// it, one read (with its attempts) or one write, close. Nothing stays open
// between two transactions.
//
// The device is a transport: /dev/i2c-N (i2c.h) or a fake monitor (fake.h).
// Time is a clock: the real one, or the tests' fake one, where sleeping only
// moves the clock forward.

#include "ddc.h"
#include "tuning.h"

#include <stdint.h>

struct vela_clock {
    int64_t (*monotonic)(void *data); // milliseconds
    int64_t (*boottime)(void *data); // milliseconds, counting suspend too
    void (*sleep)(void *data, int64_t ms);
    void *data;
};

// The real clock.
extern struct vela_clock vela_system_clock;

// Every function returns 0 or a negative errno.
struct vela_transport_ops {
    // -ENOENT (or -ENODEV, -ENXIO): no bus; -EACCES, -EPERM: no access.
    int (*open)(void *data);
    // Without waiting: -EWOULDBLOCK when another program holds the bus.
    int (*lock)(void *data);
    int (*write)(void *data, const uint8_t *bytes, size_t length);
    int (*read)(void *data, uint8_t *bytes, size_t length);
    void (*close)(void *data);
    // How the bytes travel, for --probe ("ioctl", "read/write", "fake").
    const char *(*mode)(void *data);
};

struct vela_transport {
    const struct vela_transport_ops *ops;
    void *data;
};

enum vela_result {
    VELA_RESULT_OK,
    VELA_RESULT_UNSUPPORTED, // the monitor answered: no brightness to change
    VELA_RESULT_NO_ANSWER, // no good reply in every attempt, or a write that failed
    VELA_RESULT_BUSY, // another program held the bus for the whole lock budget
    VELA_RESULT_NO_ACCESS,
    VELA_RESULT_NO_BUS,
};

const char *vela_result_name(enum vela_result result);

// What happened during one transaction, for --probe.
struct vela_attempt {
    int64_t ms; // the attempt's duration, from the request to the reply
    int error; // 0, or the errno of the request or of the reply
    enum vela_ddc_reply reply;
    uint8_t bytes[VELA_DDC_REPLY_LENGTH];
    size_t length; // bytes received (0 after an error)
};

struct vela_transaction_log {
    int open_error;
    int64_t lock_ms; // waiting for the lock
    int attempts;
    struct vela_attempt attempt[VELA_DDC_READ_ATTEMPTS];
    int64_t total_ms;
};

// Brightness (VCP 0x10) of the monitor behind `transport`. `log` may be NULL.
enum vela_result vela_ddc_read(struct vela_transport *transport, struct vela_clock *clock,
    uint16_t *value, uint16_t *max, struct vela_transaction_log *log);
enum vela_result vela_ddc_write(struct vela_transport *transport, struct vela_clock *clock,
    uint16_t value, struct vela_transaction_log *log);

#endif
