// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdint.h>

// Keep the monotonic scheduling deadline fixed across wall-clock jumps.
// Only its display timestamp changes. Keeping this arithmetic pure lets
// tests simulate discontinuous clock changes without setting system time.
static inline int64_t vela_up_wall_deadline(uint64_t deadline_boottime_us,
    uint64_t now_boottime_us, uint64_t now_realtime_us)
{
    uint64_t remaining = deadline_boottime_us > now_boottime_us
        ? deadline_boottime_us - now_boottime_us : 0;
    return (int64_t)((now_realtime_us + remaining + 999999ULL) / 1000000ULL);
}
