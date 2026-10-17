// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lock_retry_policy.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct vela_lock_retry retry = { 0 };

    for (int attempt = 1; attempt <= VELA_LOCK_RETRY_FAST_LIMIT; ++attempt) {
        assert(vela_lock_retry_begin(&retry));
        assert(retry.attempts == (unsigned)attempt);
        int delay = attempt == VELA_LOCK_RETRY_FAST_LIMIT
            ? VELA_LOCK_RETRY_COOLDOWN_MS : VELA_LOCK_RETRY_DELAY_MS;
        assert(vela_lock_retry_delay_ms(&retry) == delay);
        retry.scheduled = true;
        // Repeated lock commands while waiting must not spawn more lockers.
        assert(!vela_lock_retry_begin(&retry));
        assert(retry.attempts == (unsigned)attempt);
        vela_lock_retry_timer_fired(&retry);
    }
    // After the bounded cooldown, retry with a clean fast-attempt budget.
    assert(retry.attempts == 0);
    assert(vela_lock_retry_begin(&retry));
    assert(retry.attempts == 1);
    retry.scheduled = true;
    vela_lock_retry_registered(&retry);
    assert(!retry.scheduled && retry.attempts == 0);
    assert(vela_lock_retry_begin(&retry));
    assert(retry.attempts == 1);
    puts("lock retry policy passed");
    return 0;
}
