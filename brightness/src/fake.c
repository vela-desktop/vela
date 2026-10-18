// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fake.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// As the Mi returned it on 2026-10-17, always the same (§3).
const uint8_t vela_fake_mi_block[VELA_DDC_REPLY_LENGTH] = {
    0x2f, 0x9a, 0x2d, 0x3a, 0xb5, 0xff, 0x9d, 0x37, 0xb3, 0x63, 0x23,
};

void vela_fake_defaults(struct vela_fake_monitor *monitor)
{
    memset(monitor, 0, sizeof(*monitor));
    monitor->max = 100;
    monitor->value = 100;
    monitor->ignore_gap_lo = -1;
    monitor->ignore_gap_hi = -1;
    monitor->ddc = true;
    monitor->seed = 1;
}

// ------------------------------------------------------------ the file --

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) {
        ++text;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static bool parse_number(const char *text, long min, long max, long *out)
{
    if (!isdigit((unsigned char)*text)) {
        return false;
    }
    errno = 0;
    char *end;
    long n = strtol(text, &end, 10);
    if (errno || *end != '\0' || n < min || n > max) {
        return false;
    }
    *out = n;
    return true;
}

static bool parse_bool(const char *text, bool *out)
{
    if (strcmp(text, "yes") == 0) {
        *out = true;
    } else if (strcmp(text, "no") == 0) {
        *out = false;
    } else {
        return false;
    }
    return true;
}

// "lo-hi"
static bool parse_range(char *text, int *lo, int *hi)
{
    char *dash = strchr(text, '-');
    long a, b;
    if (!dash) {
        return false;
    }
    *dash = '\0';
    if (!parse_number(trim(text), 0, 100000, &a) || !parse_number(trim(dash + 1), 0, 100000, &b) || a > b) {
        return false;
    }
    *lo = (int)a;
    *hi = (int)b;
    return true;
}

// "<ms>:<value>, ..."
static bool parse_buttons(struct vela_fake_monitor *monitor, char *text)
{
    monitor->button_count = 0;
    for (char *item = strtok(text, ","); item; item = strtok(NULL, ",")) {
        char *colon = strchr(item, ':');
        long at, value;
        if (!colon || monitor->button_count == VELA_FAKE_BUTTONS) {
            return false;
        }
        *colon = '\0';
        if (!parse_number(trim(item), 0, 86400000, &at) || !parse_number(trim(colon + 1), 0, 65535, &value)) {
            return false;
        }
        if (monitor->button_count > 0 && at < monitor->buttons[monitor->button_count - 1].at) {
            return false;
        }
        monitor->buttons[monitor->button_count].at = at;
        monitor->buttons[monitor->button_count].value = (uint16_t)value;
        ++monitor->button_count;
    }
    return true;
}

static bool set_key(struct vela_fake_monitor *monitor, const char *key, char *value)
{
    long n;
    if (strcmp(key, "max") == 0 || strcmp(key, "value") == 0) {
        if (!parse_number(value, 0, 65535, &n)) {
            return false;
        }
        *(key[0] == 'm' ? &monitor->max : &monitor->value) = (uint16_t)n;
        return true;
    }
    struct {
        const char *key;
        int *field;
    } percents[] = {
        { "drop-writes", &monitor->drop_writes },
        { "bad-replies", &monitor->bad_replies },
        { "null-replies", &monitor->null_replies },
        { "io-errors", &monitor->io_errors },
    };
    for (size_t i = 0; i < sizeof(percents) / sizeof(percents[0]); ++i) {
        if (strcmp(key, percents[i].key) == 0) {
            if (!parse_number(value, 0, 100, &n)) {
                return false;
            }
            *percents[i].field = (int)n;
            return true;
        }
    }
    struct {
        const char *key;
        int64_t *field;
    } durations[] = {
        { "settle", &monitor->settle },
        { "asleep", &monitor->asleep },
        { "bus-busy", &monitor->bus_busy },
    };
    for (size_t i = 0; i < sizeof(durations) / sizeof(durations[0]); ++i) {
        if (strcmp(key, durations[i].key) == 0) {
            if (!parse_number(value, 0, 86400000, &n)) {
                return false;
            }
            *durations[i].field = n;
            return true;
        }
    }
    struct {
        const char *key;
        bool *field;
    } flags[] = {
        { "locked", &monitor->locked },
        { "ddc", &monitor->ddc },
        { "unsupported", &monitor->unsupported },
        { "no-bus", &monitor->no_bus },
        { "no-access", &monitor->no_access },
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        if (strcmp(key, flags[i].key) == 0) {
            return parse_bool(value, flags[i].field);
        }
    }
    if (strcmp(key, "ignore-write-gap") == 0) {
        return parse_range(value, &monitor->ignore_gap_lo, &monitor->ignore_gap_hi);
    }
    if (strcmp(key, "buttons") == 0) {
        return parse_buttons(monitor, value);
    }
    if (strcmp(key, "hang-after") == 0) {
        if (!parse_number(value, 0, 1000000, &n)) {
            return false;
        }
        monitor->hang_after = (int)n;
        return true;
    }
    if (strcmp(key, "seed") == 0) {
        if (!parse_number(value, 0, 2147483647, &n)) {
            return false;
        }
        monitor->seed = (uint64_t)n;
        return true;
    }
    return false;
}

// "card1-DP-3" matches [card1-DP-3] and [DP-3].
static bool section_matches(const char *section, const char *connector)
{
    if (strcmp(section, connector) == 0) {
        return true;
    }
    if (strncmp(connector, "card", 4) != 0) {
        return false;
    }
    const char *c = connector + 4;
    while (isdigit((unsigned char)*c)) {
        ++c;
    }
    return c > connector + 4 && *c == '-' && strcmp(section, c + 1) == 0;
}

// Calls `line` for each line without comments and blanks; stops when it
// returns false.
static bool each_line(const char *text, bool (*line)(void *data, char *line, int number), void *data)
{
    char buffer[512];
    int number = 0;
    while (*text) {
        const char *end = strchr(text, '\n');
        size_t length = end ? (size_t)(end - text) : strlen(text);
        ++number;
        if (length >= sizeof(buffer)) {
            length = sizeof(buffer) - 1;
        }
        memcpy(buffer, text, length);
        buffer[length] = '\0';
        text = end ? end + 1 : text + strlen(text);
        char *hash = strchr(buffer, '#');
        if (hash) {
            *hash = '\0';
        }
        char *content = trim(buffer);
        if (*content && !line(data, content, number)) {
            return false;
        }
    }
    return true;
}

// "[name]": the name, trimmed, or NULL.
static char *section_name(char *line)
{
    size_t length = strlen(line);
    if (line[0] != '[' || line[length - 1] != ']') {
        return NULL;
    }
    line[length - 1] = '\0';
    return trim(line + 1);
}

struct parse_state {
    struct vela_fake_monitor *monitor;
    const char *connector;
    char *error;
    size_t size;
    bool in_section;
    bool found;
};

static bool parse_line(void *data, char *line, int number)
{
    struct parse_state *state = data;
    char *name = section_name(line);
    if (name) {
        // The first matching section counts.
        state->in_section = !state->found && section_matches(name, state->connector);
        state->found = state->found || state->in_section;
        return true;
    }
    char *equals = strchr(line, '=');
    if (!equals) {
        snprintf(state->error, state->size, "line %d: expected key = value", number);
        return false;
    }
    *equals = '\0';
    char *key = trim(line);
    char *value = trim(equals + 1);
    struct vela_fake_monitor probe;
    vela_fake_defaults(&probe);
    // Every section is checked, not only the one in use: a typo anywhere is
    // an error.
    if (!set_key(state->in_section ? state->monitor : &probe, key, value)) {
        snprintf(state->error, state->size, "line %d: bad %s", number, key);
        return false;
    }
    return true;
}

int vela_fake_parse(struct vela_fake_monitor *monitor, const char *text, const char *connector,
    char *error, size_t size)
{
    vela_fake_defaults(monitor);
    struct parse_state state = { .monitor = monitor, .connector = connector, .error = error, .size = size };
    if (size > 0) {
        error[0] = '\0';
    }
    if (!each_line(text, parse_line, &state)) {
        return -EINVAL;
    }
    if (monitor->value > monitor->max) {
        snprintf(error, size, "[%s]: value above max", connector);
        return -EINVAL;
    }
    return state.found ? 0 : -ENOENT;
}

int vela_fake_load(struct vela_fake_monitor *monitor, const char *path, const char *connector,
    char *error, size_t size)
{
    FILE *file = fopen(path, "re");
    if (!file) {
        int e = errno;
        snprintf(error, size, "%s: %s", path, strerror(e));
        return -e;
    }
    char text[16384];
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[length] = '\0';
    return vela_fake_parse(monitor, text, connector, error, size);
}

struct sections_state {
    void (*section)(void *data, const char *name);
    void *data;
};

static bool section_line(void *data, char *line, int number)
{
    struct sections_state *state = data;
    char *name = section_name(line);
    if (name) {
        state->section(state->data, name);
    }
    return true;
}

void vela_fake_sections(const char *text, void (*section)(void *data, const char *name), void *data)
{
    struct sections_state state = { section, data };
    each_line(text, section_line, &state);
}

// ---------------------------------------------------------- the monitor --

static int64_t elapsed(struct vela_fake_monitor *monitor)
{
    return monitor->clock->monotonic(monitor->clock->data) - monitor->start;
}

// splitmix64: small, and the same everywhere.
static uint64_t next_random(struct vela_fake_monitor *monitor)
{
    uint64_t z = (monitor->random += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static bool happens(struct vela_fake_monitor *monitor, int percent)
{
    return percent > 0 && (int)(next_random(monitor) % 100) < percent;
}

static void record(struct vela_fake_monitor *monitor, enum vela_fake_op op, uint16_t value, bool applied)
{
    if (monitor->log_count < VELA_FAKE_LOG) {
        monitor->log[monitor->log_count] = (struct vela_fake_entry) {
            .op = op, .at = elapsed(monitor), .value = value, .applied = applied,
        };
    }
    ++monitor->log_count;
}

static void press_buttons(struct vela_fake_monitor *monitor)
{
    int64_t now = elapsed(monitor);
    while (monitor->next_button < monitor->button_count && monitor->buttons[monitor->next_button].at <= now) {
        uint16_t value = monitor->buttons[monitor->next_button++].value;
        monitor->value = value < monitor->max ? value : monitor->max;
    }
}

static bool awake(struct vela_fake_monitor *monitor)
{
    return monitor->ddc && elapsed(monitor) >= monitor->awake_at;
}

void vela_fake_start(struct vela_fake_monitor *monitor, struct vela_clock *clock)
{
    monitor->clock = clock;
    monitor->start = clock->monotonic(clock->data);
    monitor->awake_at = monitor->asleep;
    monitor->last_write = INT64_MIN;
    monitor->random = monitor->seed;
}

void vela_fake_power_on(struct vela_fake_monitor *monitor)
{
    monitor->awake_at = elapsed(monitor) + monitor->asleep;
}

static int fake_open(void *data)
{
    struct vela_fake_monitor *monitor = data;
    if (monitor->no_bus) {
        return -ENOENT;
    }
    if (monitor->no_access) {
        return -EACCES;
    }
    if (monitor->hang_after > 0 && monitor->transactions >= monitor->hang_after) {
        for (;;) {
            pause(); // a transfer stuck in the driver
        }
    }
    ++monitor->transactions;
    ++monitor->opens;
    monitor->is_open = true;
    monitor->is_locked = false;
    monitor->get_pending = false;
    record(monitor, VELA_FAKE_OPEN, 0, false);
    return 0;
}

static int fake_lock(void *data)
{
    struct vela_fake_monitor *monitor = data;
    if (elapsed(monitor) < monitor->bus_busy) {
        return -EWOULDBLOCK;
    }
    monitor->is_locked = true;
    return 0;
}

// What the host writes: a request.
static int fake_write(void *data, const uint8_t *bytes, size_t length)
{
    struct vela_fake_monitor *monitor = data;
    if (!monitor->is_open || !monitor->is_locked) {
        ++monitor->unlocked;
    }
    press_buttons(monitor);
    struct vela_ddc_request request = vela_ddc_parse_request(bytes, length);
    if (request.kind == VELA_DDC_REQUEST_GET) {
        ++monitor->gets;
        record(monitor, VELA_FAKE_GET, 0, false);
    }
    if (!awake(monitor)) {
        return -EREMOTEIO; // nobody acknowledges the address
    }
    int64_t now = elapsed(monitor);
    if (request.kind == VELA_DDC_REQUEST_GET) {
        monitor->get_pending = request.feature == VELA_DDC_BRIGHTNESS;
    } else if (request.kind == VELA_DDC_REQUEST_SET && request.feature == VELA_DDC_BRIGHTNESS) {
        ++monitor->sets;
        bool applied = true;
        if (monitor->last_write != INT64_MIN && now - monitor->last_write >= monitor->ignore_gap_lo
            && now - monitor->last_write <= monitor->ignore_gap_hi) {
            applied = false;
        } else if (happens(monitor, monitor->drop_writes) || monitor->locked || monitor->unsupported) {
            applied = false;
        }
        if (applied) {
            monitor->value = request.value < monitor->max ? request.value : monitor->max;
            ++monitor->applied;
        }
        monitor->last_write = now;
        record(monitor, VELA_FAKE_SET, request.value, applied);
    }
    return 0;
}

// What the host reads: the reply to the last Get VCP.
static int fake_read(void *data, uint8_t *bytes, size_t length)
{
    struct vela_fake_monitor *monitor = data;
    if (!monitor->is_open || !monitor->is_locked) {
        ++monitor->unlocked;
    }
    press_buttons(monitor);
    bool pending = monitor->get_pending;
    monitor->get_pending = false;
    if (!awake(monitor)) {
        return -EREMOTEIO;
    }
    int64_t now = elapsed(monitor);
    bool settling = monitor->last_write != INT64_MIN && now - monitor->last_write < monitor->settle;
    if (!pending || settling || happens(monitor, monitor->io_errors)) {
        return -EIO;
    }
    uint8_t reply[VELA_DDC_REPLY_LENGTH];
    memset(reply, 0xff, sizeof(reply));
    if (happens(monitor, monitor->bad_replies)) {
        memcpy(reply, vela_fake_mi_block, sizeof(reply));
    } else if (happens(monitor, monitor->null_replies)) {
        reply[0] = 0x6e;
        reply[1] = 0x80;
        reply[2] = 0xbe;
    } else {
        vela_ddc_encode_reply(reply, VELA_DDC_BRIGHTNESS, !monitor->unsupported, monitor->value, monitor->max);
    }
    memcpy(bytes, reply, length < sizeof(reply) ? length : sizeof(reply));
    record(monitor, VELA_FAKE_REPLY, monitor->value, false);
    return 0;
}

static void fake_close(void *data)
{
    struct vela_fake_monitor *monitor = data;
    monitor->is_open = false;
    monitor->is_locked = false;
    record(monitor, VELA_FAKE_CLOSE, 0, false);
}

static const char *fake_mode(void *data)
{
    return "fake";
}

static const struct vela_transport_ops fake_ops = {
    .open = fake_open,
    .lock = fake_lock,
    .write = fake_write,
    .read = fake_read,
    .close = fake_close,
    .mode = fake_mode,
};

struct vela_transport vela_fake_transport(struct vela_fake_monitor *monitor)
{
    return (struct vela_transport) { .ops = &fake_ops, .data = monitor };
}
