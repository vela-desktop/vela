// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lines.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Digits only, no sign, no overflow; `*text` moves past them.
static bool parse_u64(const char **text, uint64_t max, uint64_t *out)
{
    const char *c = *text;
    if (*c < '0' || *c > '9') {
        return false;
    }
    uint64_t n = 0;
    for (; *c >= '0' && *c <= '9'; ++c) {
        uint64_t digit = (uint64_t)(*c - '0');
        if (n > (max - digit) / 10) {
            return false;
        }
        n = n * 10 + digit;
    }
    *text = c;
    *out = n;
    return true;
}

// "<word> on|off" (or the two given words).
static bool parse_switch(const char *rest, const char *yes, const char *no, bool *on)
{
    if (strcmp(rest, yes) == 0) {
        *on = true;
    } else if (strcmp(rest, no) == 0) {
        *on = false;
    } else {
        return false;
    }
    return true;
}

static const char *after(const char *line, const char *word)
{
    size_t length = strlen(word);
    return strncmp(line, word, length) == 0 && line[length] == ' ' ? line + length + 1 : NULL;
}

struct vela_command vela_parse_command(const char *line)
{
    struct vela_command command = { .kind = VELA_COMMAND_INVALID };
    const char *rest;
    if (strcmp(line, "probe") == 0) {
        command.kind = VELA_COMMAND_PROBE;
    } else if ((rest = after(line, "set"))) {
        uint64_t seq, value;
        if (parse_u64(&rest, UINT64_MAX, &seq) && seq > 0 && *rest++ == ' '
            && parse_u64(&rest, UINT32_MAX, &value) && *rest == '\0') {
            command.kind = VELA_COMMAND_SET;
            command.seq = seq;
            command.value = (uint32_t)value;
        }
    } else if ((rest = after(line, "live"))) {
        if (parse_switch(rest, "on", "off", &command.on)) {
            command.kind = VELA_COMMAND_LIVE;
        }
    } else if ((rest = after(line, "power"))) {
        if (parse_switch(rest, "on", "off", &command.on)) {
            command.kind = VELA_COMMAND_POWER;
        }
    } else if ((rest = after(line, "session"))) {
        if (parse_switch(rest, "active", "inactive", &command.on)) {
            command.kind = VELA_COMMAND_SESSION;
        }
    }
    return command;
}

size_t vela_format_event(char *out, size_t size, const struct vela_event *event)
{
    int length = 0;
    switch (event->kind) {
    case VELA_EVENT_STATE:
        if (event->state == VELA_STATE_READY) {
            length = snprintf(out, size, "state ready %u %u\n", event->value, event->max);
        } else {
            length = snprintf(out, size, "state %s\n", vela_state_name(event->state));
        }
        break;
    case VELA_EVENT_VALUE:
        length = snprintf(out, size, "value %" PRIu64 " %u\n", event->seq, event->value);
        break;
    case VELA_EVENT_REFUSED:
        length = snprintf(out, size, "refused %" PRIu64 " %u\n", event->seq, event->value);
        break;
    }
    return length < 0 ? 0 : (size_t)length < size ? (size_t)length : size - 1;
}

void vela_line_reader_feed(struct vela_line_reader *reader, const char *bytes, size_t length,
    void (*line)(void *data, const char *line), void *data)
{
    for (size_t i = 0; i < length; ++i) {
        char c = bytes[i];
        if (c == '\n') {
            if (!reader->overlong) {
                reader->buffer[reader->length] = '\0';
                line(data, reader->buffer);
            }
            reader->length = 0;
            reader->overlong = false;
        } else if (reader->length == VELA_LINE_MAX) {
            reader->overlong = true;
        } else if (!reader->overlong) {
            reader->buffer[reader->length++] = c;
        }
    }
}
