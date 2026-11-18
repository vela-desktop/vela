// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela.conf as the compositor reads it (compositor/src/config.c): any bytes
// in the file, then every key looked up. The file is the user's, but it is
// also written by Settings, the shell and migrations, and read at startup.

#include "config.h"
#include "fuzz.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    vela_fuzz_config_file("vela.conf", data, size);
    struct vela_config config;
    vela_config_read(&config);
    for (int i = 0; i < config.count; ++i) {
        const char *value = vela_config_get(&config, config.entries[i].key, NULL);
        // Every key read back has a value, and it is a C string.
        if (!value) {
            __builtin_trap();
        }
        volatile size_t length = strlen(value);
        (void)length;
        (void)vela_config_flag(&config, config.entries[i].key, false);
    }
    (void)vela_config_get(&config, "", "fallback");
    vela_config_finish(&config);
    return 0;
}
