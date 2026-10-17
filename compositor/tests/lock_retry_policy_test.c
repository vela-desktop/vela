// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lock_retry_policy.h"
#include <stdlib.h>
#include <stdio.h>

// CTest builds with NDEBUG in RelWithDebInfo, so assert() would skip checks.
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAILED: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        return EXIT_FAILURE; \
    } \
} while (0)

int main(void)
{
    struct vela_lock_retry retry = { 0 };

    for (int attempt = 1; attempt <= VELA_LOCK_RETRY_FAST_LIMIT; ++attempt) {
        CHECK(vela_lock_retry_begin(&retry));
        CHECK(retry.attempts == (unsigned)attempt);
        int delay = attempt == VELA_LOCK_RETRY_FAST_LIMIT
            ? VELA_LOCK_RETRY_COOLDOWN_MS : VELA_LOCK_RETRY_DELAY_MS;
        CHECK(vela_lock_retry_delay_ms(&retry) == delay);
        retry.scheduled = true;
        // Repeated lock commands while waiting must not spawn more lockers.
        CHECK(!vela_lock_retry_begin(&retry));
        CHECK(retry.attempts == (unsigned)attempt);
        vela_lock_retry_timer_fired(&retry);
    }
    // After the bounded cooldown, retry with a clean fast-attempt budget.
    CHECK(retry.attempts == 0);
    CHECK(vela_lock_retry_begin(&retry));
    CHECK(retry.attempts == 1);
    retry.scheduled = true;
    vela_lock_retry_registered(&retry);
    CHECK(!retry.scheduled && retry.attempts == 0);
    CHECK(vela_lock_retry_begin(&retry));
    CHECK(retry.attempts == 1);
    puts("lock retry policy passed");
    return 0;
}
