// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "brightness_model.h"

#include "buffer.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const state_names[] = {
    [VELA_BRIGHTNESS_PROBING] = "probing",
    [VELA_BRIGHTNESS_READY] = "ready",
    [VELA_BRIGHTNESS_UNSUPPORTED] = "unsupported",
    [VELA_BRIGHTNESS_NO_ANSWER] = "no-answer",
    [VELA_BRIGHTNESS_NO_ACCESS] = "no-access",
    [VELA_BRIGHTNESS_NO_BUS] = "no-bus",
    [VELA_BRIGHTNESS_NOT_RESPONDING] = "not-responding",
};
#define STATES ((int)(sizeof(state_names) / sizeof(state_names[0])))

void vela_brightness_model_init(struct vela_brightness_model *model)
{
    memset(model, 0, sizeof(*model));
    model->state = VELA_BRIGHTNESS_PROBING;
}

static void forget(struct vela_brightness_model *model)
{
    model->pending = false;
    model->refused = false;
    model->known = false;
    model->confirmed = 0;
    model->target = 0;
    model->max = 0;
}

void vela_brightness_model_restart(struct vela_brightness_model *model)
{
    forget(model);
    model->state = VELA_BRIGHTNESS_PROBING;
}

bool vela_brightness_model_fail(struct vela_brightness_model *model)
{
    bool changed = model->state != VELA_BRIGHTNESS_NOT_RESPONDING || model->pending;
    model->state = VELA_BRIGHTNESS_NOT_RESPONDING;
    model->pending = false;
    model->target = model->confirmed;
    return changed;
}

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

static const char *after(const char *line, const char *word)
{
    size_t length = strlen(word);
    return strncmp(line, word, length) == 0 && line[length] == ' ' ? line + length + 1 : NULL;
}

// "<a> <b>" with nothing after.
static bool parse_pair(const char *text, uint64_t max_a, uint64_t *a, uint64_t *b)
{
    return parse_u64(&text, max_a, a) && *text++ == ' ' && parse_u64(&text, UINT32_MAX, b) && *text == '\0';
}

// What clients are shown, to tell whether a line changed it.
struct shown {
    enum vela_brightness_state state;
    uint32_t value, max;
    bool known, pending, refused;
};

static struct shown shown(const struct vela_brightness_model *model)
{
    return (struct shown) {
        .state = model->state,
        .value = model->pending ? model->target : model->confirmed,
        .max = model->max,
        .known = model->known,
        .pending = model->pending,
        .refused = model->refused,
    };
}

static bool same(struct shown a, struct shown b)
{
    return a.state == b.state && a.value == b.value && a.max == b.max && a.known == b.known
        && a.pending == b.pending && a.refused == b.refused;
}

static void ready(struct vela_brightness_model *model, uint32_t value, uint32_t max)
{
    model->state = VELA_BRIGHTNESS_READY;
    model->max = max;
    model->confirmed = value < max ? value : max;
    model->known = true;
    if (model->target > max) {
        model->target = max;
    }
    if (!model->pending) {
        model->target = model->confirmed;
    }
}

enum vela_brightness_line vela_brightness_model_line(struct vela_brightness_model *model, const char *line)
{
    if (strcmp(line, "busy") == 0) {
        return VELA_BRIGHTNESS_LINE_BUSY;
    }
    if (strcmp(line, "idle") == 0) {
        return VELA_BRIGHTNESS_LINE_IDLE;
    }
    struct shown before = shown(model);
    const char *rest;
    uint64_t a, b;
    if ((rest = after(line, "state"))) {
        const char *values = after(rest, "ready");
        if (values) {
            if (!parse_pair(values, UINT32_MAX, &a, &b) || b == 0) {
                return VELA_BRIGHTNESS_LINE_INVALID;
            }
            ready(model, (uint32_t)a, (uint32_t)b);
        } else {
            int state = 0;
            while (state < STATES && (state == VELA_BRIGHTNESS_READY || strcmp(rest, state_names[state]) != 0)) {
                ++state;
            }
            if (state == STATES) {
                return VELA_BRIGHTNESS_LINE_INVALID;
            }
            // Away from ready, the helper dropped its target, and its sets
            // until it is ready again.
            model->state = (enum vela_brightness_state)state;
            model->pending = false;
            model->target = model->confirmed;
            if (state != VELA_BRIGHTNESS_NOT_RESPONDING) {
                forget(model);
            }
        }
    } else if ((rest = after(line, "value")) || (rest = after(line, "refused"))) {
        bool refused = line[0] == 'r';
        if (!parse_pair(rest, UINT64_MAX, &a, &b)) {
            return VELA_BRIGHTNESS_LINE_INVALID;
        }
        if (!model->known) {
            return VELA_BRIGHTNESS_LINE_UNCHANGED; // before `state ready`: nothing to compare it to
        }
        model->confirmed = (uint32_t)b < model->max ? (uint32_t)b : model->max;
        // An answer to an older target updates only what the monitor has: the
        // slider stays on the newest target (§7.1). A live read carries the
        // newest seq too, but answers nothing: a refusal's note stays.
        if (a == model->seq && model->pending) {
            model->pending = false;
            model->refused = refused;
        }
        if (!model->pending) {
            model->target = model->confirmed;
        }
    } else {
        return VELA_BRIGHTNESS_LINE_INVALID;
    }
    return same(before, shown(model)) ? VELA_BRIGHTNESS_LINE_UNCHANGED : VELA_BRIGHTNESS_LINE_CHANGED;
}

bool vela_brightness_model_ready(const struct vela_brightness_model *model)
{
    return model->state == VELA_BRIGHTNESS_READY && !model->off && model->known;
}

bool vela_brightness_model_set(struct vela_brightness_model *model, double value, char *line, size_t size)
{
    line[0] = '\0';
    if (!vela_brightness_model_ready(model) || !isfinite(value) || value < 0.0 || value > 1.0) {
        return false;
    }
    uint32_t raw = (uint32_t)lround(value * model->max);
    if (raw == (model->pending ? model->target : model->confirmed)) {
        // Already what is shown: only a refusal's note goes away.
        bool had_note = model->refused;
        model->refused = false;
        return had_note;
    }
    ++model->seq;
    model->target = raw;
    model->pending = true;
    model->refused = false;
    snprintf(line, size, "set %" PRIu64 " %" PRIu32 "\n", model->seq, raw);
    return true;
}

const char *vela_brightness_model_state_name(const struct vela_brightness_model *model)
{
    return model->off ? "off" : state_names[model->state];
}

void vela_brightness_model_json(const struct vela_brightness_model *model, const char *name,
    const char *description, const char *mechanism, struct vela_buffer *json)
{
    vela_buffer_append(json, "{\"name\":");
    vela_buffer_append_json(json, name);
    vela_buffer_append(json, ",\"description\":");
    vela_buffer_append_json(json, description ? description : "");
    vela_buffer_appendf(json, ",\"mechanism\":\"%s\",\"state\":\"%s\",\"value\":", mechanism,
        vela_brightness_model_state_name(model));
    if (model->known && model->max > 0) {
        uint32_t value = model->pending ? model->target : model->confirmed;
        vela_buffer_appendf(json, "%.6g", (double)value / model->max);
    } else {
        vela_buffer_append(json, "null");
    }
    vela_buffer_appendf(json, ",\"pending\":%s,\"refused\":%s,\"max\":%" PRIu32 ",\"seq\":%" PRIu64 "}",
        model->pending ? "true" : "false", model->refused ? "true" : "false", model->known ? model->max : 0,
        model->seq);
}
