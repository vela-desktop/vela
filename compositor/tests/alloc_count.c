// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The counting wrappers of alloc_count.h. They call the real helpers, so
// the allocation and its failure behave exactly as without them.

#include "alloc_count.h"

#include "alloc/alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *__real_vela_alloc_at(size_t count, size_t size, const char *file, int line);
void *__real_vela_grow_size_at(void *items, size_t *capacity, size_t needed, size_t size, const char *file,
    int line);
void *__real_vela_grow_int_at(void *items, int *capacity, long long needed, size_t size, const char *file,
    int line);
char *__real_vela_strdup_at(const char *s, const char *file, int line);
char *__real_vela_strndup_at(const char *s, size_t n, const char *file, int line);

void *__wrap_vela_alloc_at(size_t count, size_t size, const char *file, int line);
void *__wrap_vela_grow_size_at(void *items, size_t *capacity, size_t needed, size_t size, const char *file,
    int line);
void *__wrap_vela_grow_int_at(void *items, int *capacity, long long needed, size_t size, const char *file,
    int line);
char *__wrap_vela_strdup_at(const char *s, const char *file, int line);
char *__wrap_vela_strndup_at(const char *s, size_t n, const char *file, int line);
char *__wrap_vela_asprintf_at(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static struct vela_alloc_count counts;
static int log_fd = -2; // -2: not opened yet; -1: no log

struct vela_alloc_count vela_alloc_count_get(void)
{
    return counts;
}

// The descriptor VELA_TEST_ALLOC_LOG_FD names, opened for appending by the
// test, or -1. The counter opens nothing itself. The descriptor is kept from
// the compositor's children.
static int inherited_log(void)
{
    const char *text = getenv("VELA_TEST_ALLOC_LOG_FD");
    if (!text || !*text) {
        return -1;
    }
    char *end;
    errno = 0;
    long fd = strtol(text, &end, 10);
    if (errno || *end || fd < 0 || fd > INT_MAX) {
        return -1;
    }
    int flags = fcntl((int)fd, F_GETFD);
    if (flags < 0 || fcntl((int)fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return -1;
    }
    return (int)fd;
}

static void note(const char *kind, const char *file, int line, size_t count, size_t size)
{
    ++counts.allocations;
    if (log_fd == -2) {
        log_fd = inherited_log();
    }
    if (log_fd < 0) {
        return;
    }
    char text[512];
    int length = snprintf(text, sizeof(text), "%s %s:%d %zux%zu\n", kind, file, line, count, size);
    if (length > 0) {
        // One write per line with O_APPEND: lines never interleave.
        ssize_t written = write(log_fd, text, (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1);
        (void)written;
    }
}

void *__wrap_vela_alloc_at(size_t count, size_t size, const char *file, int line)
{
    note("alloc", file, line, count, size);
    return __real_vela_alloc_at(count, size, file, line);
}

void *__wrap_vela_grow_size_at(void *items, size_t *capacity, size_t needed, size_t size, const char *file,
    int line)
{
    size_t before = *capacity;
    void *grown = __real_vela_grow_size_at(items, capacity, needed, size, file, line);
    if (grown != items || *capacity != before) {
        note("grow", file, line, *capacity, size);
    } else {
        ++counts.checks;
    }
    return grown;
}

void *__wrap_vela_grow_int_at(void *items, int *capacity, long long needed, size_t size, const char *file,
    int line)
{
    int before = *capacity;
    void *grown = __real_vela_grow_int_at(items, capacity, needed, size, file, line);
    if (grown != items || *capacity != before) {
        note("grow", file, line, (size_t)*capacity, size);
    } else {
        ++counts.checks;
    }
    return grown;
}

char *__wrap_vela_strdup_at(const char *s, const char *file, int line)
{
    note("strdup", file, line, strlen(s) + 1, 1);
    return __real_vela_strdup_at(s, file, line);
}

char *__wrap_vela_strndup_at(const char *s, size_t n, const char *file, int line)
{
    note("strndup", file, line, n, 1);
    return __real_vela_strndup_at(s, n, file, line);
}

// A variadic function cannot hand its arguments to __real_: the same steps
// as vela_asprintf_at, with the block from the real vela_alloc_at.
char *__wrap_vela_asprintf_at(const char *file, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int length = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (length < 0) {
        vela_alloc_misuse("format failed", file, line);
    }
    note("asprintf", file, line, (size_t)length + 1, 1);
    char *text = __real_vela_alloc_at((size_t)length + 1, 1, file, line);
    va_start(args, fmt);
    vsnprintf(text, (size_t)length + 1, fmt, args);
    va_end(args);
    return text;
}
