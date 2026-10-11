// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output_key.h"
#include <stdio.h>

void vela_output_key_format(char *out, size_t size, const char *make,
    const char *model, const char *serial, const char *fallback)
{
    if (!out || size == 0) {
        return;
    }
    out[0] = '\0';
    size_t used = 0;
    const char *parts[] = { make, model, serial };
    for (size_t i = 0; i < 3; ++i) {
        if (!parts[i] || !*parts[i] || used == size - 1) {
            continue;
        }
        int n = snprintf(out + used, size - used, "%s%s", used ? " " : "", parts[i]);
        if (n < 0) {
            return;
        }
        if ((size_t)n >= size - used) {
            break;
        }
        used += (size_t)n;
    }
    if (!out[0] && fallback) {
        snprintf(out, size, "%s", fallback);
    }
}
