// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "keymap_options.h"

#include <stdio.h>
#include <string.h>

// Whether `options` (comma-separated) holds an option of group `group`
// ("grp:").
static bool has_group(const char *options, const char *group)
{
    const size_t length = strlen(group);
    for (const char *at = options; *at;) {
        at += strspn(at, " ,");
        if (strncmp(at, group, length) == 0) {
            return true;
        }
        at += strcspn(at, ",");
    }
    return false;
}

bool vela_keymap_options(const char *layout, const char *system, const char *vela, char *out, size_t size)
{
    if (size == 0) {
        return false;
    }
    const char *options = vela ? vela : system ? system : "";
    const bool switching = layout && strchr(layout, ',') && !has_group(options, "grp:");
    const int written = snprintf(
        out, size, "%s%s%s", options, switching && *options ? "," : "", switching ? "grp:win_space_toggle" : "");
    if (written < 0 || (size_t)written >= size) {
        out[0] = '\0';
        return false;
    }
    return true;
}
