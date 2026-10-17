// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scheduler.h"

#include <string.h>

// "Long before anything": far enough in the past that adding an interval
// can't overflow.
#define LONG_AGO (INT64_MIN / 4)

// A `no-answer` probe is tried again 2, 10 and 30 s after the first (§5.5).
static const int64_t probe_retry_ms[] = { 2000, 8000, 20000 };
#define PROBE_RETRIES ((int)(sizeof(probe_retry_ms) / sizeof(probe_retry_ms[0])))

const char *vela_state_name(enum vela_state state)
{
    switch (state) {
    case VELA_STATE_PROBING:
        return "probing";
    case VELA_STATE_READY:
        return "ready";
    case VELA_STATE_UNSUPPORTED:
        return "unsupported";
    case VELA_STATE_NO_ANSWER:
        return "no-answer";
    case VELA_STATE_NO_ACCESS:
        return "no-access";
    case VELA_STATE_NO_BUS:
        return "no-bus";
    case VELA_STATE_NOT_RESPONDING:
        break;
    }
    return "not-responding";
}

static int64_t max64(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

static int64_t min64(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

// Ready, or ready once and not answering now: brightness can be written.
static bool usable(const struct vela_scheduler *scheduler)
{
    return scheduler->state == VELA_STATE_READY || scheduler->state == VELA_STATE_NOT_RESPONDING;
}

static void emit(struct vela_scheduler *scheduler, enum vela_event_kind kind, uint64_t seq, uint16_t value)
{
    struct vela_event event = {
        .kind = kind,
        .state = scheduler->state,
        .seq = seq,
        .value = value,
        .max = scheduler->max,
    };
    scheduler->emit(scheduler->emit_data, &event);
}

static void enter(struct vela_scheduler *scheduler, enum vela_state state)
{
    if (scheduler->state == state) {
        return;
    }
    scheduler->state = state;
    emit(scheduler, VELA_EVENT_STATE, 0, scheduler->confirmed);
}

// A state without brightness: the target and the reads are dropped.
static void leave(struct vela_scheduler *scheduler, enum vela_state state)
{
    scheduler->pending = false;
    scheduler->probe_due = false;
    scheduler->wake_at = VELA_NEVER;
    scheduler->known = false;
    enter(scheduler, state);
}

static void wake(struct vela_scheduler *scheduler, int64_t now)
{
    scheduler->wake_at = now + VELA_WAKE_MS;
    scheduler->grace_until = now + VELA_WAKE_GRACE_MS;
}

void vela_scheduler_init(struct vela_scheduler *scheduler, int64_t now, int64_t boot,
    void (*emit)(void *data, const struct vela_event *event), void *data)
{
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->emit = emit;
    scheduler->emit_data = data;
    scheduler->state = VELA_STATE_PROBING;
    scheduler->last_write = LONG_AGO;
    scheduler->last_read = LONG_AGO;
    scheduler->write_after = LONG_AGO;
    scheduler->power = true;
    scheduler->session = true;
    scheduler->wake_at = VELA_NEVER;
    scheduler->grace_until = LONG_AGO;
    scheduler->probe_due = true;
    scheduler->probe_at = now;
    scheduler->boot_offset = boot - now;
}

void vela_scheduler_clock(struct vela_scheduler *scheduler, int64_t now, int64_t boot)
{
    int64_t offset = boot - now;
    if (offset - scheduler->boot_offset >= VELA_SUSPEND_MS) {
        wake(scheduler, now);
    }
    scheduler->boot_offset = offset;
}

void vela_scheduler_set(struct vela_scheduler *scheduler, uint64_t seq, uint32_t value, int64_t now)
{
    if (seq <= scheduler->last_seq) {
        return; // an old or repeated target
    }
    scheduler->last_seq = seq;
    if (!usable(scheduler)) {
        return;
    }
    uint16_t target = value < scheduler->max ? (uint16_t)value : scheduler->max;
    if (scheduler->pending && target == scheduler->target) {
        scheduler->seq = seq;
        return;
    }
    if (!scheduler->pending && scheduler->known && !scheduler->dirty && target == scheduler->confirmed) {
        emit(scheduler, VELA_EVENT_VALUE, seq, target); // already there
        return;
    }
    scheduler->pending = true;
    scheduler->seq = seq;
    scheduler->target = target;
    scheduler->written = false;
    scheduler->rewrites = 0;
    scheduler->since = now;
    scheduler->answered = false;
}

void vela_scheduler_live(struct vela_scheduler *scheduler, bool on)
{
    scheduler->live = on;
}

void vela_scheduler_power(struct vela_scheduler *scheduler, bool on, int64_t now)
{
    if (on && !scheduler->power) {
        wake(scheduler, now);
    }
    scheduler->power = on;
}

void vela_scheduler_session(struct vela_scheduler *scheduler, bool active, int64_t now)
{
    if (active && !scheduler->session) {
        wake(scheduler, now);
    }
    scheduler->session = active;
}

void vela_scheduler_probe(struct vela_scheduler *scheduler, int64_t now)
{
    scheduler->probe_due = true;
    scheduler->probe_at = now;
    scheduler->probe_retries = 0;
}

static struct vela_action wait_until(int64_t at)
{
    return (struct vela_action) { .kind = VELA_ACTION_WAIT, .at = at };
}

static struct vela_action start(struct vela_scheduler *scheduler, struct vela_action action)
{
    scheduler->busy = true;
    scheduler->doing = action;
    return action;
}

static struct vela_action read_now(struct vela_scheduler *scheduler, enum vela_read_kind kind)
{
    return start(scheduler, (struct vela_action) { .kind = VELA_ACTION_READ, .read = kind });
}

// The budget ran out (§5.4): refused if the monitor answered, otherwise it
// isn't responding.
static void give_up(struct vela_scheduler *scheduler)
{
    scheduler->pending = false;
    if (scheduler->answered) {
        emit(scheduler, VELA_EVENT_REFUSED, scheduler->seq, scheduler->confirmed);
    } else {
        enter(scheduler, VELA_STATE_NOT_RESPONDING);
    }
}

struct vela_action vela_scheduler_next(struct vela_scheduler *scheduler, int64_t now)
{
    if (scheduler->busy || !scheduler->power || !scheduler->session) {
        return wait_until(VELA_NEVER);
    }
    if (scheduler->wake_at != VELA_NEVER) {
        return now >= scheduler->wake_at ? read_now(scheduler, VELA_READ_WAKE) : wait_until(scheduler->wake_at);
    }
    if (!usable(scheduler)) {
        if (!scheduler->probe_due) {
            return wait_until(VELA_NEVER);
        }
        return now >= scheduler->probe_at ? read_now(scheduler, VELA_READ_PROBE) : wait_until(scheduler->probe_at);
    }
    if (scheduler->pending) {
        int64_t end = scheduler->since + VELA_BUDGET_MS;
        if (now >= end) {
            give_up(scheduler);
            return vela_scheduler_next(scheduler, now);
        }
        if (!scheduler->written) {
            if (now < scheduler->write_after) {
                return wait_until(min64(scheduler->write_after, end));
            }
            return start(scheduler, (struct vela_action) { .kind = VELA_ACTION_WRITE, .value = scheduler->target });
        }
        if (now < scheduler->readback_at) {
            return wait_until(min64(scheduler->readback_at, end));
        }
        return read_now(scheduler, VELA_READ_BACK);
    }
    if (scheduler->probe_due) {
        return now >= scheduler->probe_at ? read_now(scheduler, VELA_READ_PROBE) : wait_until(scheduler->probe_at);
    }
    if (scheduler->live) {
        int64_t at = max64(scheduler->last_read, scheduler->last_write) + VELA_LIVE_MS;
        return now >= at ? read_now(scheduler, VELA_READ_LIVE) : wait_until(at);
    }
    return wait_until(VELA_NEVER);
}

static void good_read(struct vela_scheduler *scheduler, enum vela_read_kind kind, uint16_t value, uint16_t max,
    int64_t now)
{
    scheduler->failed_reads = 0;
    if (scheduler->wake_at != VELA_NEVER || now < scheduler->grace_until) {
        // Awake: a target waiting since the wake-up gets its whole budget.
        scheduler->since = max64(scheduler->since, now);
    }
    scheduler->wake_at = VELA_NEVER;
    scheduler->grace_until = LONG_AGO;
    if (kind == VELA_READ_PROBE) {
        scheduler->probe_due = false;
        scheduler->probe_retries = 0;
    }
    bool was_ready = scheduler->state == VELA_STATE_READY;
    bool changed = !scheduler->known || value != scheduler->confirmed;
    bool new_max = max != scheduler->max;
    scheduler->confirmed = value;
    scheduler->known = true;
    scheduler->dirty = false;
    scheduler->max = max;
    if (!was_ready) {
        enter(scheduler, VELA_STATE_READY); // carries the value
    } else if (new_max) {
        emit(scheduler, VELA_EVENT_STATE, 0, value);
    }
    if (scheduler->pending) {
        if (scheduler->target > max) {
            scheduler->target = max;
        }
        if (!scheduler->written) {
            return; // a wake-up read before the first write: write as planned
        }
        scheduler->answered = true;
        if (value == scheduler->target) {
            scheduler->pending = false;
            emit(scheduler, VELA_EVENT_VALUE, scheduler->seq, value);
        } else if (kind != VELA_READ_BACK) {
            scheduler->written = false; // the write was lost while asleep: not a rewrite
        } else if (scheduler->rewrites < VELA_REWRITES) {
            ++scheduler->rewrites;
            scheduler->written = false;
        } else {
            scheduler->pending = false;
            emit(scheduler, VELA_EVENT_REFUSED, scheduler->seq, value);
        }
        return;
    }
    if (was_ready && changed && !new_max) {
        emit(scheduler, VELA_EVENT_VALUE, scheduler->last_seq, value);
    }
}

void vela_scheduler_read_done(struct vela_scheduler *scheduler, enum vela_result result, uint16_t value,
    uint16_t max, int64_t now)
{
    enum vela_read_kind kind = scheduler->doing.read;
    scheduler->busy = false;
    scheduler->last_read = now;
    switch (result) {
    case VELA_RESULT_OK:
        good_read(scheduler, kind, value, max, now);
        return;
    case VELA_RESULT_UNSUPPORTED:
        scheduler->failed_reads = 0;
        leave(scheduler, VELA_STATE_UNSUPPORTED);
        return;
    case VELA_RESULT_NO_ACCESS:
        leave(scheduler, VELA_STATE_NO_ACCESS);
        return;
    case VELA_RESULT_NO_BUS:
        leave(scheduler, VELA_STATE_NO_BUS);
        return;
    case VELA_RESULT_BUSY:
        // Another program has the bus: not a failure, try again later.
        if (kind == VELA_READ_WAKE) {
            scheduler->wake_at = now + VELA_BUSY_RETRY_MS;
        } else if (kind == VELA_READ_PROBE) {
            scheduler->probe_at = now + VELA_BUSY_RETRY_MS;
        } else if (kind == VELA_READ_BACK) {
            scheduler->readback_at = now + VELA_BUSY_RETRY_MS;
        }
        return;
    case VELA_RESULT_NO_ANSWER:
        break;
    }
    if (now < scheduler->grace_until) {
        // Waking up (the computer may have slept during this very read): not
        // a failure, and the wake-up read comes first.
        if (kind == VELA_READ_WAKE) {
            scheduler->wake_at = now + VELA_WAKE_MS;
        } else if (kind == VELA_READ_BACK) {
            scheduler->readback_at = now + VELA_READBACK_MS;
        }
        return;
    }
    if (kind == VELA_READ_WAKE) {
        scheduler->wake_at = VELA_NEVER;
    }
    if (!usable(scheduler)) {
        enter(scheduler, VELA_STATE_NO_ANSWER);
        if (kind == VELA_READ_PROBE && scheduler->probe_retries < PROBE_RETRIES) {
            scheduler->probe_at = now + probe_retry_ms[scheduler->probe_retries++];
        } else if (kind == VELA_READ_PROBE) {
            scheduler->probe_due = false; // until something changes (§5.5)
        }
        return;
    }
    if (kind == VELA_READ_BACK) {
        scheduler->readback_at = now + VELA_READBACK_MS;
    } else if (kind == VELA_READ_PROBE) {
        scheduler->probe_due = false;
    }
    if (++scheduler->failed_reads >= VELA_FAILED_READS && scheduler->state == VELA_STATE_READY) {
        scheduler->pending = false;
        enter(scheduler, VELA_STATE_NOT_RESPONDING);
    }
}

void vela_scheduler_write_done(struct vela_scheduler *scheduler, enum vela_result result, int64_t now)
{
    uint16_t value = scheduler->doing.value;
    scheduler->busy = false;
    switch (result) {
    case VELA_RESULT_OK:
        scheduler->last_write = now;
        scheduler->write_after = now + VELA_WRITE_INTERVAL_MS;
        scheduler->dirty = true;
        if (scheduler->pending && value == scheduler->target) {
            scheduler->written = true;
            scheduler->readback_at = now + VELA_READBACK_MS;
        }
        return;
    case VELA_RESULT_NO_ANSWER:
        // Nobody acknowledged: try again after the interval, within the
        // budget.
        scheduler->last_write = now;
        scheduler->write_after = now + VELA_WRITE_INTERVAL_MS;
        scheduler->dirty = true;
        scheduler->written = false;
        return;
    case VELA_RESULT_BUSY:
        scheduler->write_after = now + VELA_BUSY_RETRY_MS;
        return;
    case VELA_RESULT_NO_ACCESS:
        leave(scheduler, VELA_STATE_NO_ACCESS);
        return;
    case VELA_RESULT_NO_BUS:
        leave(scheduler, VELA_STATE_NO_BUS);
        return;
    case VELA_RESULT_UNSUPPORTED:
        break;
    }
    leave(scheduler, VELA_STATE_UNSUPPORTED);
}
