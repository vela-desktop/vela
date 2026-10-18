// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_LINES_H
#define VELA_BRIGHTNESS_LINES_H

// The lines between the compositor and vela-brightness
// (docs/brightness.md §5.6). Pure.
//
// Compositor → helper: set <seq> <raw>, live on|off, power on|off,
// session active|inactive, probe.
// Helper → compositor: state <state> (ready <raw> <max>), value <seq> <raw>,
// refused <seq> <raw>, busy, idle.

#include "scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VELA_LINE_MAX 128

enum vela_command_kind {
    VELA_COMMAND_INVALID,
    VELA_COMMAND_SET,
    VELA_COMMAND_LIVE,
    VELA_COMMAND_POWER,
    VELA_COMMAND_SESSION,
    VELA_COMMAND_PROBE,
};

struct vela_command {
    enum vela_command_kind kind;
    uint64_t seq;
    uint32_t value;
    bool on; // live, power, session
};

// One line without its newline.
struct vela_command vela_parse_command(const char *line);

// The line for an event, with its newline; returns its length.
size_t vela_format_event(char *out, size_t size, const struct vela_event *event);

// Splits what arrives on the socket into lines. A line longer than
// VELA_LINE_MAX is dropped whole.
struct vela_line_reader {
    char buffer[VELA_LINE_MAX + 1];
    size_t length;
    bool overlong;
};

void vela_line_reader_feed(struct vela_line_reader *reader, const char *bytes, size_t length,
    void (*line)(void *data, const char *line), void *data);

#endif
