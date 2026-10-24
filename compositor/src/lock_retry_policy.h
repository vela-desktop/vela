// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VELA_LOCK_RETRY_POLICY_H
#define VELA_LOCK_RETRY_POLICY_H

#include <stdbool.h>

#define VELA_LOCK_RETRY_FAST_LIMIT 5
#define VELA_LOCK_RETRY_DELAY_MS 5000
#define VELA_LOCK_RETRY_COOLDOWN_MS 60000

// Pure registration-retry policy: no compositor, timers, or GPU required.
// A scheduled timeout blocks repeated lock commands from launching duplicates.
struct vela_lock_retry {
    unsigned attempts;
    bool scheduled;
};

static inline bool vela_lock_retry_begin(struct vela_lock_retry *retry)
{
    if (retry->scheduled) {
        return false;
    }
    ++retry->attempts;
    return true;
}

static inline int vela_lock_retry_delay_ms(const struct vela_lock_retry *retry)
{
    return retry->attempts >= VELA_LOCK_RETRY_FAST_LIMIT
        ? VELA_LOCK_RETRY_COOLDOWN_MS : VELA_LOCK_RETRY_DELAY_MS;
}

static inline void vela_lock_retry_timer_fired(struct vela_lock_retry *retry)
{
    retry->scheduled = false;
    if (retry->attempts >= VELA_LOCK_RETRY_FAST_LIMIT) {
        retry->attempts = 0;
    }
}

static inline void vela_lock_retry_registered(struct vela_lock_retry *retry)
{
    retry->attempts = 0;
    retry->scheduled = false;
}

#endif
