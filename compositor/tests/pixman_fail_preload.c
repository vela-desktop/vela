// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// LD_PRELOAD shim for the functional tests: makes one pixman region operation
// fail for lack of memory, for real. While the chosen operation runs, this
// thread's malloc/calloc/realloc return NULL, so pixman takes its genuine
// failure path (a "broken" region and a FALSE result).
//
// VELA_TEST_PIXMAN_FAIL_DIR is an inherited descriptor (a number) of the
// test's directory; its file "pixman-fail" is the trigger, containing
// "<pid> <operation> <skip> <id>", such as "1234 union_rect 2 7". Calls of
// that operation in that process that need memory are let through `skip`
// times; the next one fails, then the shim deletes the file and prints
// "pixman-fail: injected <operation> failure". Calls that need no memory (one
// rectangle) never count. A new <id> starts counting again. Other processes
// inheriting LD_PRELOAD are never affected (the pid must match).

#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <errno.h>
#include <fcntl.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *pointer, size_t size);

// initial-exec: reading it must never allocate (dynamic TLS could, inside
// malloc itself).
static __thread bool failing __attribute__((tls_model("initial-exec")));
static __thread bool counting __attribute__((tls_model("initial-exec")));
static __thread unsigned long allocations __attribute__((tls_model("initial-exec")));

// Allocating calls let through for the current trigger id.
static long trigger_id = -1;
static long skipped;

void *malloc(size_t size)
{
    if (failing) {
        errno = ENOMEM;
        return NULL;
    }
    if (counting) {
        ++allocations;
    }
    return __libc_malloc(size);
}

void *calloc(size_t count, size_t size)
{
    if (failing) {
        errno = ENOMEM;
        return NULL;
    }
    if (counting) {
        ++allocations;
    }
    return __libc_calloc(count, size);
}

void *realloc(void *pointer, size_t size)
{
    if (failing) {
        errno = ENOMEM;
        return NULL;
    }
    if (counting) {
        ++allocations;
    }
    return __libc_realloc(pointer, size);
}

static bool next_number(char **cursor, long *value)
{
    char *end = NULL;
    *value = strtol(*cursor, &end, 10);
    if (end == *cursor) {
        return false;
    }
    *cursor = end;
    return true;
}

// The calls to let through before failing, or -1 when not armed for this
// operation. No stdio and no heap: this runs on every wrapped call.
// The test directory's descriptor, or -1. Only a number comes from the
// environment: the file names are fixed.
static int trigger_directory(void)
{
    const char *text = getenv("VELA_TEST_PIXMAN_FAIL_DIR");
    char *end = NULL;
    long fd = text && *text ? strtol(text, &end, 10) : -1;
    return end && !*end && fd >= 0 && fd < 1024 ? (int)fd : -1;
}

static long armed(const char *operation)
{
    int directory = trigger_directory();
    if (directory < 0) {
        return -1;
    }
    int fd = openat(directory, "pixman-fail", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    char text[96];
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    text[n] = '\0';
    char *cursor = text;
    long pid = 0;
    if (!next_number(&cursor, &pid) || pid != (long)getpid() || *cursor != ' ') {
        return -1;
    }
    ++cursor;
    size_t length = strlen(operation);
    if (strncmp(cursor, operation, length) != 0 || cursor[length] != ' ') {
        return -1;
    }
    cursor += length;
    long skip = 0, id = 0;
    if (!next_number(&cursor, &skip) || !next_number(&cursor, &id) || skip < 0) {
        return -1;
    }
    if (id != trigger_id) {
        trigger_id = id;
        skipped = 0;
    }
    return skip;
}

static pixman_bool_t finish(const char *operation, pixman_bool_t ok)
{
    failing = false;
    if (!ok) {
        unlinkat(trigger_directory(), "pixman-fail", 0);
        fprintf(stderr, "pixman-fail: injected %s failure\n", operation);
        if (getenv("VELA_TEST_PIXMAN_FAIL_BACKTRACE")) {
            void *frames[16];
            backtrace_symbols_fd(frames, backtrace(frames, 16), STDERR_FILENO);
        }
    }
    return ok;
}

typedef pixman_bool_t (*binary_op)(pixman_region32_t *, const pixman_region32_t *, const pixman_region32_t *);
typedef pixman_bool_t (*rect_op)(pixman_region32_t *, const pixman_region32_t *, int, int, unsigned, unsigned);

// Let `skip` allocating calls through, then fail the next allocating one.
#define INJECT(operation, call)                                                \
    do {                                                                       \
        long skip = armed(operation);                                          \
        if (skip < 0) {                                                        \
            return call;                                                       \
        }                                                                      \
        if (skipped < skip) {                                                  \
            allocations = 0;                                                   \
            counting = true;                                                   \
            pixman_bool_t ok = call;                                           \
            counting = false;                                                  \
            if (allocations > 0) {                                             \
                ++skipped;                                                     \
            }                                                                  \
            return ok;                                                         \
        }                                                                      \
        failing = true;                                                        \
        return finish(operation, call);                                        \
    } while (0)

// Resolved before any failure is armed: dlsym may allocate.
static void *real_op(void **cache, const char *name)
{
    if (!*cache) {
        *cache = dlsym(RTLD_NEXT, name);
    }
    return *cache;
}

static pixman_bool_t binary(const char *operation, const char *symbol, void **cache, pixman_region32_t *dest,
    const pixman_region32_t *a, const pixman_region32_t *b)
{
    binary_op real = (binary_op)real_op(cache, symbol);
    INJECT(operation, real(dest, a, b));
}

pixman_bool_t pixman_region32_union(pixman_region32_t *dest, const pixman_region32_t *a,
    const pixman_region32_t *b)
{
    static void *cache;
    return binary("union", "pixman_region32_union", &cache, dest, a, b);
}

pixman_bool_t pixman_region32_intersect(pixman_region32_t *dest, const pixman_region32_t *a,
    const pixman_region32_t *b)
{
    static void *cache;
    return binary("intersect", "pixman_region32_intersect", &cache, dest, a, b);
}

pixman_bool_t pixman_region32_subtract(pixman_region32_t *dest, const pixman_region32_t *a,
    const pixman_region32_t *b)
{
    static void *cache;
    return binary("subtract", "pixman_region32_subtract", &cache, dest, a, b);
}

pixman_bool_t pixman_region32_copy(pixman_region32_t *dest, const pixman_region32_t *source)
{
    static void *cache;
    typedef pixman_bool_t (*copy_op)(pixman_region32_t *, const pixman_region32_t *);
    copy_op real = (copy_op)real_op(&cache, "pixman_region32_copy");
    INJECT("copy", real(dest, source));
}

pixman_bool_t pixman_region32_union_rect(pixman_region32_t *dest, const pixman_region32_t *source, int x, int y,
    unsigned int width, unsigned int height)
{
    static void *cache;
    rect_op real = (rect_op)real_op(&cache, "pixman_region32_union_rect");
    INJECT("union_rect", real(dest, source, x, y, width, height));
}
