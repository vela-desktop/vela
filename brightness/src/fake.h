// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_FAKE_H
#define VELA_BRIGHTNESS_FAKE_H

// Fake monitors (docs/brightness.md §12): a transport that answers DDC/CI
// byte for byte, with the faults measured on real monitors (§3), so the
// protocol, the attempts and the scheduler run as they do on hardware.
// Chosen with VELA_BRIGHTNESS_FAKE=<file>, a section per output:
//
//   # the LG of §3
//   [HDMI-A-1]
//   value = 100
//   ignore-write-gap = 5-190
//
// A section matches the connector by name, with or without the card
// ("card1-DP-3" or "DP-3"), so headless outputs (HEADLESS-1) get one too.
// Every key, with its default:
//
//   max = 100, value = 100
//   ignore-write-gap = <lo>-<hi>   a write lo to hi ms after another is ignored (the LG)
//   drop-writes = 0                percent of writes lost at random (the Mi)
//   bad-replies = 0                percent of replies that are the Mi's fixed block
//   null-replies = 0               percent of replies that are a null message
//   io-errors = 0                  percent of replies that fail with EIO
//   settle = 0                     ms after a write during which replies fail with EIO
//   asleep = 0                     ms without answers at start and after `power on`
//   buttons = <ms>:<value>, ...    the monitor's buttons change the value, ms after start
//   locked = no                    writes are accepted and ignored (HDR, picture modes)
//   ddc = yes                      no: nothing ever answers (DDC/CI off in the menu)
//   unsupported = no               the monitor answers that it has no brightness
//   no-bus = no, no-access = no    the device doesn't exist, or can't be opened
//   bus-busy = 0                   ms from start during which another program holds the bus
//   hang-after = 0                 the transaction after this many never ends
//   seed = 1                       for the random faults: every run is the same

#include "transfer.h"

#include <stdbool.h>
#include <stdint.h>

#define VELA_FAKE_BUTTONS 8
#define VELA_FAKE_LOG 256

enum vela_fake_op {
    VELA_FAKE_OPEN,
    VELA_FAKE_CLOSE,
    VELA_FAKE_GET, // a Get VCP request
    VELA_FAKE_SET, // a Set VCP request: `value` asked, `applied` or not
    VELA_FAKE_REPLY, // a reply read
};

struct vela_fake_entry {
    enum vela_fake_op op;
    int64_t at; // ms since start
    uint16_t value;
    bool applied;
};

struct vela_fake_monitor {
    // What the monitor is (the file).
    uint16_t max;
    uint16_t value;
    int ignore_gap_lo;
    int ignore_gap_hi; // < 0: none
    int drop_writes;
    int bad_replies;
    int null_replies;
    int io_errors;
    int64_t settle;
    int64_t asleep;
    struct {
        int64_t at;
        uint16_t value;
    } buttons[VELA_FAKE_BUTTONS];
    int button_count;
    bool locked;
    bool ddc;
    bool unsupported;
    bool no_bus;
    bool no_access;
    int64_t bus_busy;
    int hang_after;
    uint64_t seed;

    // While it runs.
    struct vela_clock *clock;
    int64_t start;
    int64_t awake_at;
    int64_t last_write; // INT64_MIN: none yet
    uint64_t random;
    int next_button;
    bool is_open;
    bool is_locked;
    bool get_pending;
    int transactions;

    // What happened, for the tests: counts and the latest entries.
    int opens;
    int sets;
    int applied;
    int gets;
    int unlocked; // requests sent without holding the device's lock
    struct vela_fake_entry log[VELA_FAKE_LOG];
    int log_count; // may exceed VELA_FAKE_LOG: only the first ones are kept
};

void vela_fake_defaults(struct vela_fake_monitor *monitor);

// Reads `connector`'s section of a fake file's text into `monitor`.
// 0: found; -ENOENT: no section for it; -EINVAL: a bad line, described in
// `error`.
int vela_fake_parse(struct vela_fake_monitor *monitor, const char *text, const char *connector,
    char *error, size_t size);
// The same, from a file. -errno when it can't be read.
int vela_fake_load(struct vela_fake_monitor *monitor, const char *path, const char *connector,
    char *error, size_t size);

// Every section of a fake file's text, for --probe: calls `section` once per
// name in the order of the file.
void vela_fake_sections(const char *text, void (*section)(void *data, const char *name), void *data);

void vela_fake_start(struct vela_fake_monitor *monitor, struct vela_clock *clock);
// The compositor turned the monitor back on: it sleeps for `asleep` again.
void vela_fake_power_on(struct vela_fake_monitor *monitor);
struct vela_transport vela_fake_transport(struct vela_fake_monitor *monitor);

// The fixed block that about half the Mi's reads return (§3).
extern const uint8_t vela_fake_mi_block[VELA_DDC_REPLY_LENGTH];

#endif
