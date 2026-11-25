// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// LD_PRELOAD shim for session recovery and descriptor-pressure tests:
// failures on the supervisor's restart path and command watch registration.
//
// VELA_TEST_FAIL_DIR is an inherited descriptor (a number) of the test's
// directory, which holds the triggers:
// "setenv": while it exists, setenv() of the variable named in it fails
//   with ENOMEM. A second word "once" deletes the file after the first
//   failure, so only one restart child is affected.
// "fstatat": while it exists, fstatat() of a name starting with
//   "vela-locked-" (the lock marker) fails with EIO.
// "command-watch": contains a command socket path; while present,
//   registering an accepted connection on that socket fails with EMFILE.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>

// The test directory's descriptor, or -1. Only a number comes from the
// environment: the file names are fixed.
static int trigger_directory(void)
{
    const char *text = getenv("VELA_TEST_FAIL_DIR");
    char *end = NULL;
    long fd = text && *text ? strtol(text, &end, 10) : -1;
    return end && !*end && fd >= 0 && fd < 1024 ? (int)fd : -1;
}

// The trigger file's first line, or false when it does not exist. No heap.
static bool trigger(const char *name, char *text, size_t size)
{
    int directory = trigger_directory();
    if (directory < 0) {
        return false;
    }
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC);
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

struct wl_event_source *wl_event_loop_add_fd(struct wl_event_loop *loop, int fd, uint32_t mask,
    wl_event_loop_fd_func_t func, void *data)
{
    static struct wl_event_source *(*real)(struct wl_event_loop *, int, uint32_t, wl_event_loop_fd_func_t, void *);
    if (!real) {
        real = (void *)dlsym(RTLD_NEXT, "wl_event_loop_add_fd");
    }
    char path[128];
    struct sockaddr_un address = { 0 };
    socklen_t length = sizeof(address);
    int listening = 1;
    socklen_t listening_length = sizeof(listening);
    if (trigger("command-watch", path, sizeof(path))
        && getsockname(fd, (struct sockaddr *)&address, &length) == 0
        && length <= sizeof(address) && address.sun_family == AF_UNIX
        && getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &listening_length) == 0
        && !listening) {
        address.sun_path[sizeof(address.sun_path) - 1] = '\0';
        if (strcmp(address.sun_path, path) == 0) {
            errno = EMFILE;
            return NULL;
        }
    }
    return real(loop, fd, mask, func, data);
}

int setenv(const char *name, const char *value, int overwrite)
{
    static int (*real)(const char *, const char *, int);
    if (!real) {
        real = (int (*)(const char *, const char *, int))dlsym(RTLD_NEXT, "setenv");
    }
    char text[128];
    if (name && trigger("setenv", text, sizeof(text))) {
        char *space = strchr(text, ' ');
        bool once = space && strcmp(space + 1, "once") == 0;
        if (space) {
            *space = '\0';
        }
        if (strcmp(text, name) == 0) {
            if (once) {
                unlinkat(trigger_directory(), "setenv", 0);
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
    return name && strncmp(name, "vela-locked-", 12) == 0 && trigger("fstatat", text, sizeof(text));
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
