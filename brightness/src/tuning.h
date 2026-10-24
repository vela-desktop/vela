// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_TUNING_H
#define VELA_BRIGHTNESS_TUNING_H

// Every timing of vela-brightness, in one place (docs/brightness.md §5.2,
// §5.4, §5.5), in milliseconds. They are starting points, tuned against the
// monitors measured in §3.

// One DDC/CI transaction (§5.2).
#define VELA_DDC_REPLY_WAIT_MS 40 // between a request and its reply (the standard's)
#define VELA_DDC_READ_ATTEMPTS 6 // with 4, a read of the Mi still failed now and then
#define VELA_DDC_RETRY_MS 50 // between two attempts of a read
#define VELA_DDC_LOCK_RETRY_MS 50 // another program holds the bus: try again after
#define VELA_DDC_LOCK_BUDGET_MS 1000 // ... for this long, then the bus is busy

// Writing a value (§5.4).
#define VELA_WRITE_INTERVAL_MS 250 // between two writes: the LG ignores a write 5-190 ms after another
#define VELA_READBACK_MS 500 // from the last write to the read back: two intervals, a slider at rest
#define VELA_REWRITES 4 // a target written again at most this many times
#define VELA_BACKLIGHT_WRITE_INTERVAL_MS 0 // a backlight takes every value of a drag
#define VELA_BACKLIGHT_READBACK_MS 100 // ... and has it at once
#define VELA_BACKLIGHT_OLED_WAIT_MS 20 // from a low write to reading actual_brightness (§5.3)
#define VELA_BUDGET_MS 8000 // ... and for at most this long

// Reading (§5.5).
#define VELA_LIVE_MS 1000 // live sync: one read this often, and not this soon after a write
#define VELA_FAILED_READS 3 // failed reads in a row that make a ready output not-responding
#define VELA_BUSY_RETRY_MS 1000 // a read skipped because the bus was busy is tried again after
#define VELA_WAKE_MS 2000 // after a wake-up: nothing for this long, then a read
#define VELA_WAKE_GRACE_MS 10000 // ... and failures don't count for this long
#define VELA_SUSPEND_MS 1000 // the boot clock gaining this much on the monotonic one is a suspend

#endif
