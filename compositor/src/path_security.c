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

static bool path_has_component_after(const char *path)
{
    while (*path) {
        while (*path == '/') {
            ++path;
        }
        if (!*path) {
            return false;
        }
        const char *component = path;
        while (*path && *path != '/') {
            ++path;
        }
        size_t length = (size_t)(path - component);
        if (!(length == 1 && component[0] == '.')) {
            return true;
        }
    }
    return false;
}

int vela_runtime_dir_open(const char *path)
{
    if (!path || *path != '/') {
        errno = EINVAL;
        return -1;
    }

    // Start from a trusted constant path, then resolve the environment-provided
    // path one component at a time. This keeps untrusted input out of open()
    // entirely while still accepting ordinary XDG aliases such as //, ./ and
    // trailing slashes.
    int fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    const char *p = path;
    while (*p) {
        while (*p == '/') {
            ++p;
        }
        if (!*p) {
            break;
        }

        const char *component = p;
        while (*p && *p != '/') {
            ++p;
        }
        size_t length = (size_t)(p - component);

        if (length == 1 && component[0] == '.') {
            continue;
        }
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            close(fd);
            errno = EINVAL;
            return -1;
        }

        char *name = strndup(component, length);
        if (!name) {
            close(fd);
            return -1;
        }

        // Preserve the previous semantics: parent symlinks may be part of a
        // valid absolute XDG path, but the runtime directory itself must not
        // be a symlink.
        bool final = !path_has_component_after(p);
        int flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
        if (final) {
            flags |= O_NOFOLLOW;
        }
        int next = openat(fd, name, flags);
        free(name);
        if (next < 0) {
            close(fd);
            return -1;
        }
        close(fd);
        fd = next;
    }

    // Validate the inode we actually reached. The descriptor is then reused
    // for socket and lock operations, so there is no check-then-reopen race.
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
