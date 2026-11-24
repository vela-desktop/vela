// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "custom.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kind_names[] = { "app", "command", "open" };

const char *vela_custom_kind_name(enum vela_custom_kind kind)
{
    return (size_t)kind < sizeof(kind_names) / sizeof(kind_names[0]) ? kind_names[kind] : "";
}

// "https:", "mailto:", "file:": a scheme, then a colon.
static bool url(const char *text, size_t length)
{
    if (length == 0 || !isalpha((unsigned char)text[0])) {
        return false;
    }
    for (size_t i = 1; i < length; ++i) {
        const char c = text[i];
        if (c == ':') {
            return i + 1 < length;
        }
        if (!isalnum((unsigned char)c) && c != '+' && c != '.' && c != '-') {
            return false;
        }
    }
    return false;
}

bool vela_custom_target_valid(enum vela_custom_kind kind, const char *target, size_t length)
{
    if (!target || length == 0 || length > VELA_CUSTOM_LINE_MAX) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)target[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    // No surrounding spaces: they would not survive a read.
    if (target[0] == ' ' || target[length - 1] == ' ') {
        return false;
    }
    switch (kind) {
    case VELA_CUSTOM_APP: {
        static const char suffix[] = ".desktop";
        const size_t n = sizeof(suffix) - 1;
        return length > n && memcmp(target + length - n, suffix, n) == 0 && !memchr(target, '/', length)
            && !memchr(target, ' ', length);
    }
    case VELA_CUSTOM_COMMAND: return true;
    case VELA_CUSTOM_OPEN: return target[0] == '/' || url(target, length);
    }
    return false;
}

bool vela_custom_line_parse(const char *line, size_t length, struct vela_custom_line *out)
{
    if (!line || length == 0 || length > VELA_CUSTOM_LINE_MAX || line[0] == '#') {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)line[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    const char *eq = memchr(line, '=', length);
    if (!eq || eq == line) {
        return false;
    }
    char combo[128];
    const size_t combo_length = (size_t)(eq - line);
    if (combo_length >= sizeof(combo)) {
        return false;
    }
    memcpy(combo, line, combo_length);
    combo[combo_length] = '\0';
    struct vela_custom_line parsed;
    if (!vela_combo_parse(combo, &parsed.combo)) {
        return false;
    }
    const char *rest = eq + 1;
    const size_t rest_length = length - combo_length - 1;
    const char *space = memchr(rest, ' ', rest_length);
    if (!space) {
        return false;
    }
    const size_t kind_length = (size_t)(space - rest);
    bool known = false;
    for (size_t k = 0; k < sizeof(kind_names) / sizeof(kind_names[0]); ++k) {
        if (strlen(kind_names[k]) == kind_length && memcmp(rest, kind_names[k], kind_length) == 0) {
            parsed.kind = (enum vela_custom_kind)k;
            known = true;
        }
    }
    parsed.target = space + 1;
    parsed.target_length = rest_length - kind_length - 1;
    if (!known || !vela_custom_target_valid(parsed.kind, parsed.target, parsed.target_length)) {
        return false;
    }
    *out = parsed;
    return true;
}

bool vela_custom_path(char *out, size_t size)
{
    const char *config = getenv("XDG_CONFIG_HOME");
    int written;
    if (config && config[0] == '/') {
        written = snprintf(out, size, "%s/vela/shortcuts.conf", config);
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') {
            return false;
        }
        written = snprintf(out, size, "%s/.config/vela/shortcuts.conf", home);
    }
    return written > 0 && (size_t)written < size;
}
