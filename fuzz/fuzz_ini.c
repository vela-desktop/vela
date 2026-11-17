// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The .ini/.desktop reader (vela_ini_read, used for .desktop files of any
// installed app) and desktop.conf (vela_desktop_conf_read) on any bytes.

#include "config.h"
#include "desktop_conf.h"
#include "fuzz.h"

#include <string.h>

static void seen(void *data, const char *text)
{
    (void)data;
    (void)strlen(text);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char path[4096];
    if (!vela_fuzz_config_file("app.desktop", data, size, path, sizeof(path))) {
        return 0;
    }
    static const char *const keys[] = { "Name", "Icon", "Exec", "" };
    char values[4][256];
    (void)vela_ini_read(path, "Desktop Entry", keys, values, 4);
    for (int i = 0; i < 4; ++i) {
        // Each value fits its buffer with its terminator.
        if (strnlen(values[i], sizeof(values[i])) >= sizeof(values[i])) {
            __builtin_trap();
        }
    }
    const struct vela_desktop_conf_reader reader = { .desktop = seen, .sticky_app = seen, .data = NULL };
    (void)vela_desktop_conf_read(path, &reader);
    return 0;
}
