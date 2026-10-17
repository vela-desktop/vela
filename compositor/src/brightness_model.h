// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_MODEL_H
#define VELA_BRIGHTNESS_MODEL_H

// One output's brightness as the compositor owns it (docs/brightness.md
// §7.1): what its vela-brightness says, what clients asked for, and what they
// are shown. Pure: brightness.c feeds it the helper's lines and the clients'
// requests, and sends on what it returns.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vela_buffer;

enum vela_brightness_state {
    VELA_BRIGHTNESS_PROBING,
    VELA_BRIGHTNESS_READY,
    VELA_BRIGHTNESS_UNSUPPORTED,
    VELA_BRIGHTNESS_NO_ANSWER,
    VELA_BRIGHTNESS_NO_ACCESS,
    VELA_BRIGHTNESS_NO_BUS,
    VELA_BRIGHTNESS_NOT_RESPONDING,
};

struct vela_brightness_model {
    enum vela_brightness_state state; // the helper's
    bool off; // the compositor turned the output off (shown as "off")
    uint64_t seq; // of the latest target; grows for the output's whole life
    uint32_t target; // the latest value asked for
    uint32_t confirmed; // the latest value read from the monitor
    uint32_t max;
    bool known; // `confirmed` and `max` were read
    bool pending; // `target` not yet confirmed
    bool refused; // the latest target was refused; cleared by the next change
};

// What a line from the helper was.
enum vela_brightness_line {
    VELA_BRIGHTNESS_LINE_INVALID,
    VELA_BRIGHTNESS_LINE_UNCHANGED, // valid, nothing a client sees changed
    VELA_BRIGHTNESS_LINE_CHANGED,
    VELA_BRIGHTNESS_LINE_BUSY, // a transaction started (the watchdog, §6)
    VELA_BRIGHTNESS_LINE_IDLE, // ... and ended
};

void vela_brightness_model_init(struct vela_brightness_model *model);

// A new helper process: probing again, nothing pending. `seq` keeps growing,
// so nothing the old process said can be taken for an answer.
void vela_brightness_model_restart(struct vela_brightness_model *model);

// The helper is gone or hung (watchdog, crashes): not-responding.
bool vela_brightness_model_fail(struct vela_brightness_model *model);

// A line from the helper, without its newline (§5.6).
enum vela_brightness_line vela_brightness_model_line(struct vela_brightness_model *model, const char *line);

// A client asks for `value`, 0 to 1. Returns true when something changed;
// then `line` holds the line for the helper ("set <seq> <raw>\n"), or is empty
// when only the shown state changed. False: not settable now (not ready, off),
// or the value is out of range.
bool vela_brightness_model_set(struct vela_brightness_model *model, double value, char *line, size_t size);

// Whether a slider can move it.
bool vela_brightness_model_ready(const struct vela_brightness_model *model);

// The state clients see: the helper's, or "off".
const char *vela_brightness_model_state_name(const struct vela_brightness_model *model);

// One output for the `brightness` answer:
// {"name":...,"state":...,"value":0.8,"pending":false,"refused":false,"max":100,"seq":3}.
// `value` is the target while pending, the confirmed value otherwise (§7.1),
// null until the monitor answered.
void vela_brightness_model_json(const struct vela_brightness_model *model, const char *name,
    struct vela_buffer *json);

#endif
