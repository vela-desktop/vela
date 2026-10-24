// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A clock where sleeping only moves time forward: the tests run hours of
// monitor time in microseconds.

extern "C" {
#include "transfer.h"
}

#include <cstdint>

struct FakeClock {
    int64_t monotonic = 1000000;
    int64_t boot = 5000000;
    vela_clock clock {};

    FakeClock()
    {
        clock.monotonic = [](void* data) { return static_cast<FakeClock*>(data)->monotonic; };
        clock.boottime = [](void* data) { return static_cast<FakeClock*>(data)->boot; };
        clock.sleep = [](void* data, int64_t ms) { static_cast<FakeClock*>(data)->advance(ms); };
        clock.data = this;
    }
    FakeClock(const FakeClock&) = delete;
    FakeClock& operator=(const FakeClock&) = delete;

    void advance(int64_t ms)
    {
        monotonic += ms;
        boot += ms;
    }

    // The computer sleeps: only the boot clock counts it.
    void suspend(int64_t ms) { boot += ms; }
};
