// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_SCHEDULER_H
#define VELA_BRIGHTNESS_SCHEDULER_H

// When vela-brightness touches its monitor, and what it tells the
// compositor (docs/brightness.md §5.4, §5.5). Pure: the caller gives the
// time, asks what to do next, does it (one transaction, transfer.h) and
// reports the result. Times in milliseconds on the monotonic clock.
//
// - A new target replaces the old one; writes are at least
//   VELA_WRITE_INTERVAL_MS apart; once no new target came for
//   VELA_READBACK_MS after the last write, a read back confirms it or writes
//   it again, VELA_REWRITES times within VELA_BUDGET_MS, then `refused`.
// - Live sync reads once every VELA_LIVE_MS, never while a target is
//   pending nor right after a write.
// - Power on, session active and a system suspend (the boot clock gaining on
//   the monotonic one) are wake-ups: nothing for VELA_WAKE_MS, then a read,
//   with failures not counted for VELA_WAKE_GRACE_MS.

#include "transfer.h"

#include <stdbool.h>
#include <stdint.h>

#define VELA_NEVER INT64_MAX

enum vela_state {
    VELA_STATE_PROBING,
    VELA_STATE_READY,
    VELA_STATE_UNSUPPORTED,
    VELA_STATE_NO_ANSWER,
    VELA_STATE_NO_ACCESS,
    VELA_STATE_NO_BUS,
    VELA_STATE_NOT_RESPONDING,
};

const char *vela_state_name(enum vela_state state);

enum vela_action_kind {
    VELA_ACTION_WAIT, // nothing until `at` (VELA_NEVER: until a line comes)
    VELA_ACTION_READ,
    VELA_ACTION_WRITE, // `value`
};

enum vela_read_kind {
    VELA_READ_PROBE,
    VELA_READ_BACK,
    VELA_READ_LIVE,
    VELA_READ_WAKE,
};

struct vela_action {
    enum vela_action_kind kind;
    enum vela_read_kind read;
    uint16_t value;
    int64_t at;
};

// What the compositor is told (the lines of §5.6).
enum vela_event_kind {
    VELA_EVENT_STATE, // `state`; READY carries `value` and `max`
    VELA_EVENT_VALUE, // `value` confirmed, for target `seq`
    VELA_EVENT_REFUSED, // the target `seq` was refused: the monitor kept `value`
};

struct vela_event {
    enum vela_event_kind kind;
    enum vela_state state;
    uint64_t seq;
    uint16_t value;
    uint16_t max;
};

struct vela_scheduler {
    void (*emit)(void *data, const struct vela_event *event);
    void *emit_data;

    enum vela_state state;
    uint16_t max;
    bool known; // `confirmed` was read at least once
    uint16_t confirmed; // the value last read from the monitor
    bool dirty; // written since that read
    uint64_t last_seq; // the newest seq received

    // The target (§5.4).
    bool pending;
    uint64_t seq;
    uint16_t target;
    bool written; // written, waiting for the read back
    int rewrites;
    int64_t since; // the budget's start
    bool answered; // a read back answered during this target
    int64_t write_after;
    int64_t readback_at;

    int64_t last_write; // the end of the last write
    int64_t last_read; // the end of the last read
    int failed_reads;

    bool live;
    bool power;
    bool session;
    int64_t wake_at; // VELA_NEVER: no wake-up read due
    int64_t grace_until;

    bool probe_due;
    int64_t probe_at;
    int probe_retries;

    int64_t boot_offset; // boot clock - monotonic clock

    bool busy; // between next() returning a transaction and its done()
    struct vela_action doing;
};

void vela_scheduler_init(struct vela_scheduler *scheduler, int64_t now, int64_t boot,
    void (*emit)(void *data, const struct vela_event *event), void *data);

// Before anything else, with both clocks: notices a suspend.
void vela_scheduler_clock(struct vela_scheduler *scheduler, int64_t now, int64_t boot);

// The compositor's lines. `seq` grows with every target, from 1.
void vela_scheduler_set(struct vela_scheduler *scheduler, uint64_t seq, uint32_t value, int64_t now);
void vela_scheduler_live(struct vela_scheduler *scheduler, bool on);
void vela_scheduler_power(struct vela_scheduler *scheduler, bool on, int64_t now);
void vela_scheduler_session(struct vela_scheduler *scheduler, bool active, int64_t now);
void vela_scheduler_probe(struct vela_scheduler *scheduler, int64_t now);

// What to do now. A READ or WRITE must be done, then reported.
struct vela_action vela_scheduler_next(struct vela_scheduler *scheduler, int64_t now);
void vela_scheduler_read_done(struct vela_scheduler *scheduler, enum vela_result result, uint16_t value,
    uint16_t max, int64_t now);
void vela_scheduler_write_done(struct vela_scheduler *scheduler, enum vela_result result, int64_t now);

#endif
