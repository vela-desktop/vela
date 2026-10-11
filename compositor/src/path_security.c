// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "path_security.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool vela_relative_path_safe(const char *path)
{
    if (!path || !*path || *path == '/') {
        return false;
    }
    const char *part = path;
    for (const unsigned char *p = (const unsigned char *)path;; ++p) {
        if (*p && (*p < 32 || *p == 127)) {
            return false;
        }
        if (!*p || *p == '/') {
            size_t length = (size_t)((const char *)p - part);
            if (!length || (length == 1 && part[0] == '.')
                || (length == 2 && part[0] == '.' && part[1] == '.')) {
                return false;
            }
            if (!*p) {
                return true;
            }
            part = (const char *)(p + 1);
        }
    }
}

bool vela_path_component_safe(const char *name)
{
    return vela_relative_path_safe(name) && strchr(name, '/') == NULL;
}

bool vela_runtime_dir_private(const char *path)
{
    if (!path || *path != '/' || !vela_relative_path_safe(path + 1)) {
        return false;
    }
    struct stat info;
    // Unlike stat(), lstat() rejects a symlink to an otherwise-private dir.
    return lstat(path, &info) == 0 && S_ISDIR(info.st_mode)
        && info.st_uid == geteuid() && (info.st_mode & 077) == 0;
}

bool vela_lock_flag_path_for_runtime(const char *runtime_dir, const char *display,
    char *out, size_t size)
{
    if (!runtime_dir || !out || !size || *runtime_dir != '/'
        || !vela_relative_path_safe(runtime_dir + 1)
        || !vela_path_component_safe(display)) {
        return false;
    }
    int n = snprintf(out, size, "%s/vela-locked-%s", runtime_dir, display);
    return n > 0 && (size_t)n < size;
}
