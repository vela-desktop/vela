// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "path_security.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
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

int vela_runtime_dir_open(const char *path)
{
    if (!path || *path != '/') {
        errno = EINVAL;
        return -1;
    }

    // Trailing slashes are valid in XDG_RUNTIME_DIR, but would defeat
    // O_NOFOLLOW on a symlink to a directory.
    char *normalized = strdup(path);
    if (!normalized) {
        return -1;
    }
    size_t length = strlen(normalized);
    while (length > 1 && normalized[length - 1] == '/') {
        normalized[--length] = '\0';
    }
    int fd = open(normalized, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    free(normalized);
    if (fd < 0) {
        return -1;
    }

    // Validate the directory we actually opened: never check a pathname and
    // then reopen it, since it could be replaced between those operations.
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode)
        || info.st_uid != geteuid() || (info.st_mode & 077) != 0) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}

bool vela_runtime_dir_private(const char *path)
{
    int fd = vela_runtime_dir_open(path);
    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
}

bool vela_lock_flag_path_for_runtime(const char *runtime_dir, const char *display,
    char *out, size_t size)
{
    if (!runtime_dir || !out || !size || *runtime_dir != '/'
        || !vela_path_component_safe(display)) {
        return false;
    }
    // Keep accepting equivalent absolute XDG paths (including trailing '/').
    size_t length = strlen(runtime_dir);
    while (length > 1 && runtime_dir[length - 1] == '/') {
        --length;
    }
    if (length > INT_MAX) {
        return false;
    }
    int n = snprintf(out, size, "%.*s%svela-locked-%s", (int)length,
        runtime_dir, length == 1 ? "" : "/", display);
    return n > 0 && (size_t)n < size;
}

bool vela_lock_marker_create(const char *path)
{
    if (!path || !*path) {
        return false;
    }
    char temporary[PATH_MAX + 8];
    int n = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(temporary)) {
        return false;
    }
    // A unique owner-only file avoids following a pre-existing marker symlink.
    int fd = mkstemp(temporary);
    if (fd < 0) {
        return false;
    }
    bool ok = close(fd) == 0;
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}
