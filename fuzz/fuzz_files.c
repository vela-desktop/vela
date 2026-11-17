// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fuzz.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

bool vela_fuzz_config_file(const char *name, const uint8_t *data, size_t size, char *path, size_t path_size)
{
    static char home[64];
    if (!home[0]) {
        char pattern[] = "/tmp/vela-fuzz-XXXXXX";
        if (!mkdtemp(pattern)) {
            return false;
        }
        snprintf(home, sizeof(home), "%s", pattern);
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/vela", home);
        if (mkdir(dir, 0700) != 0 || setenv("XDG_CONFIG_HOME", home, 1) != 0) {
            return false;
        }
    }
    int written = snprintf(path, path_size, "%s/vela/%s", home, name);
    if (written < 0 || (size_t)written >= path_size) {
        return false;
    }
    FILE *file = fopen(path, "we");
    if (!file) {
        return false;
    }
    bool ok = fwrite(data, 1, size, file) == size;
    return fclose(file) == 0 && ok;
}
