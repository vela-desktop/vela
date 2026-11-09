// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// LD_PRELOAD shim for the session recovery tests (tests/functional/
// test_session.py): makes two libc calls of the supervisor's restart path
// fail as they could under memory pressure.
//
// VELA_TEST_SETENV_FAIL=<file>: while <file> exists, setenv() of the
//   variable named in it fails with ENOMEM. A second word "once" deletes the
//   file after the first failure, so only one restart child is affected.
// VELA_TEST_FSTATAT_FAIL=<file>: while <file> exists, fstatat() of a name
//   starting with "vela-locked-" (the lock marker) fails with EIO.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The trigger file's first line, or false when it does not exist. No heap.
static bool trigger(const char *variable, char *text, size_t size)
{
    const char *path = getenv(variable);
    if (!path || !*path) {
        return false;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    ssize_t n = read(fd, text, size - 1);
    close(fd);
    text[n > 0 ? n : 0] = '\0';
    char *newline = strchr(text, '\n');
    if (newline) {
        *newline = '\0';
    }
    return true;
}

int setenv(const char *name, const char *value, int overwrite)
{
    static int (*real)(const char *, const char *, int);
    if (!real) {
        real = (int (*)(const char *, const char *, int))dlsym(RTLD_NEXT, "setenv");
    }
    char text[128];
    if (name && trigger("VELA_TEST_SETENV_FAIL", text, sizeof(text))) {
        char *space = strchr(text, ' ');
        bool once = space && strcmp(space + 1, "once") == 0;
        if (space) {
            *space = '\0';
        }
        if (strcmp(text, name) == 0) {
            if (once) {
                unlink(getenv("VELA_TEST_SETENV_FAIL"));
            }
            static const char message[] = "libc-fail: injected setenv failure\n";
            (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
            errno = ENOMEM;
            return -1;
        }
    }
    return real(name, value, overwrite);
}

static bool marker_fails(const char *name)
{
    char text[8];
    return name && strncmp(name, "vela-locked-", 12) == 0 && trigger("VELA_TEST_FSTATAT_FAIL", text, sizeof(text));
}

int fstatat(int dirfd, const char *name, struct stat *info, int flags)
{
    static int (*real)(int, const char *, struct stat *, int);
    if (!real) {
        real = (int (*)(int, const char *, struct stat *, int))dlsym(RTLD_NEXT, "fstatat");
    }
    if (marker_fails(name)) {
        errno = EIO;
        return -1;
    }
    return real(dirfd, name, info, flags);
}

int fstatat64(int dirfd, const char *name, struct stat64 *info, int flags)
{
    static int (*real)(int, const char *, struct stat64 *, int);
    if (!real) {
        real = (int (*)(int, const char *, struct stat64 *, int))dlsym(RTLD_NEXT, "fstatat64");
    }
    if (marker_fails(name)) {
        errno = EIO;
        return -1;
    }
    return real(dirfd, name, info, flags);
}
