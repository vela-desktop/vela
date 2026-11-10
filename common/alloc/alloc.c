// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The only file allowed to call the libc allocators directly
// (scripts/check-allocations.py).

#include "alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { VELA_GROW_INITIAL = 8 };

// ---- The allocation-free writer -------------------------------------------
//
// Everything below runs when the heap may be exhausted or corrupt: a fixed
// stack buffer, write(2), open/read for the two memory figures, no stdio.

struct line {
    char text[640];
    size_t length;
};

static void put(struct line *out, const char *text)
{
    for (; text && *text && out->length < sizeof(out->text) - 1; ++text) {
        out->text[out->length++] = *text;
    }
}

static void put_number(struct line *out, unsigned long long value)
{
    char digits[24];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n && out->length < sizeof(out->text) - 1) {
        out->text[out->length++] = digits[--n];
    }
}

// The value in kB of the "<key>:" line of a /proc file, or -1. The files read
// here are a few kilobytes and the keys are near their start.
static long long proc_kib(const char *path, const char *key)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    char text[4096];
    size_t length = 0;
    while (length < sizeof(text) - 1) {
        ssize_t n = read(fd, text + length, sizeof(text) - 1 - length);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        length += (size_t)n;
    }
    close(fd);
    text[length] = '\0';
    size_t key_length = strlen(key);
    for (const char *at = text; at && *at;) {
        const char *next = strchr(at, '\n');
        if (strncmp(at, key, key_length) != 0 || at[key_length] != ':') {
            at = next ? next + 1 : NULL;
            continue;
        }
        const char *c = at + key_length + 1;
        while (*c == ' ' || *c == '\t') {
            ++c;
        }
        if (*c < '0' || *c > '9') {
            return -1;
        }
        long long value = 0;
        for (; *c >= '0' && *c <= '9'; ++c) {
            if (value > (LLONG_MAX - 9) / 10) {
                return -1;
            }
            value = value * 10 + (*c - '0');
        }
        return value;
    }
    return -1;
}

static void put_memory(struct line *out)
{
    long long rss = proc_kib("/proc/self/status", "VmRSS");
    if (rss >= 0) {
        put(out, "; rss ");
        put_number(out, (unsigned long long)rss / 1024);
        put(out, " MiB");
    }
    long long available = proc_kib("/proc/meminfo", "MemAvailable");
    if (available >= 0) {
        put(out, "; available ");
        put_number(out, (unsigned long long)available / 1024);
        put(out, " MiB");
    }
}

static void put_site(struct line *out, const char *file, int line)
{
    put(out, " at ");
    put(out, file ? file : "?");
    put(out, ":");
    put_number(out, line > 0 ? (unsigned long long)line : 0);
}

VELA_ALLOC_NORETURN static void finish(struct line *out)
{
    out->text[out->length++] = '\n';
    for (size_t written = 0; written < out->length;) {
        ssize_t n = write(STDERR_FILENO, out->text + written, out->length - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        written += (size_t)n;
    }
    abort();
}

void vela_oom(size_t count, size_t size, const char *file, int line)
{
    struct line out = { .length = 0 };
    put(&out, "vela: out of memory (");
    put_number(&out, count);
    put(&out, " x ");
    put_number(&out, size);
    put(&out, " bytes)");
    put_site(&out, file, line);
    put_memory(&out);
    finish(&out);
}

VELA_ALLOC_NORETURN static void size_overflow(size_t a, const char *operation, size_t b,
    const char *file, int line)
{
    struct line out = { .length = 0 };
    put(&out, "vela: size overflow (");
    put_number(&out, a);
    put(&out, operation);
    put_number(&out, b);
    put(&out, ")");
    put_site(&out, file, line);
    put_memory(&out);
    finish(&out);
}

void vela_alloc_misuse(const char *what, const char *file, int line)
{
    struct line out = { .length = 0 };
    put(&out, "vela: allocation misuse (");
    put(&out, what);
    put(&out, ")");
    put_site(&out, file, line);
    finish(&out);
}

void vela_must_failed(const char *expression, const char *file, int line)
{
    struct line out = { .length = 0 };
    put(&out, "vela: out of memory (");
    put(&out, expression);
    put(&out, ")");
    put_site(&out, file, line);
    put_memory(&out);
    finish(&out);
}

const void *vela_must_at(const void *value, const char *expression, const char *file, int line)
{
    if (!value) {
        vela_must_failed(expression, file, line);
    }
    return value;
}

// ---- The helpers ----------------------------------------------------------

void *vela_alloc_at(size_t count, size_t size, const char *file, int line)
{
    size_t bytes;
    if (__builtin_mul_overflow(count, size, &bytes)) {
        vela_oom(count, size, file, line);
    }
    // Never NULL, even for an empty block: memcpy(p, src, 0) with p == NULL
    // is undefined behaviour.
    void *block = bytes ? calloc(count, size) : calloc(1, 1);
    if (!block) {
        vela_oom(count, size, file, line);
    }
    return block;
}

// The shared core: grows `items` from `capacity` elements to at least
// `needed`, never beyond `limit` elements, and zeroes the new tail.
static void *grow(void *items, size_t *capacity, size_t needed, size_t size, size_t limit,
    const char *file, int line)
{
    if (size == 0) {
        vela_alloc_misuse("element size 0", file, line);
    }
    if (!items && *capacity) {
        vela_alloc_misuse("capacity without an array", file, line);
    }
    if (items && needed <= *capacity) {
        return items;
    }
    if (needed > limit) {
        vela_oom(needed, size, file, line);
    }
    size_t grown = *capacity ? *capacity : VELA_GROW_INITIAL;
    while (grown < needed) {
        grown = grown > limit / 2 ? limit : grown * 2;
    }
    size_t bytes;
    if (__builtin_mul_overflow(grown, size, &bytes)) {
        // Doubling overshot what fits in memory: the exact request may still.
        grown = needed;
        if (__builtin_mul_overflow(grown, size, &bytes)) {
            vela_oom(needed, size, file, line);
        }
    }
    void *bigger = realloc(items, bytes);
    if (!bigger) {
        vela_oom(grown, size, file, line);
    }
    size_t old_bytes = *capacity * size; // no overflow: it was allocated
    memset((char *)bigger + old_bytes, 0, bytes - old_bytes);
    *capacity = grown;
    return bigger;
}

void *vela_grow_size_at(void *items, size_t *capacity, size_t needed, size_t size,
    const char *file, int line)
{
    return grow(items, capacity, needed, size, SIZE_MAX, file, line);
}

void *vela_grow_int_at(void *items, int *capacity, long long needed, size_t size,
    const char *file, int line)
{
    if (needed < 0) {
        vela_alloc_misuse("negative count", file, line);
    }
    if (*capacity < 0) {
        vela_alloc_misuse("negative capacity", file, line);
    }
    // Beyond INT_MAX, the core's limit reports it as out of memory.
    size_t wide = (size_t)*capacity;
    void *grown = grow(items, &wide, (size_t)needed, size, INT_MAX, file, line);
    *capacity = (int)wide;
    return grown;
}

char *vela_strdup_at(const char *s, const char *file, int line)
{
    if (!s) {
        vela_alloc_misuse("NULL string", file, line);
    }
    size_t length = strlen(s);
    char *copy = vela_alloc_at(length + 1, 1, file, line);
    memcpy(copy, s, length);
    return copy;
}

char *vela_strndup_at(const char *s, size_t n, const char *file, int line)
{
    if (!s) {
        vela_alloc_misuse("NULL string", file, line);
    }
    size_t length = strnlen(s, n);
    char *copy = vela_alloc_at(length + 1, 1, file, line);
    memcpy(copy, s, length);
    return copy;
}

char *vela_asprintf_at(const char *file, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int length = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (length < 0) {
        // An encoding error or more than INT_MAX bytes: the format is wrong.
        vela_alloc_misuse("format failed", file, line);
    }
    char *text = vela_alloc_at((size_t)length + 1, 1, file, line);
    va_start(args, fmt);
    vsnprintf(text, (size_t)length + 1, fmt, args);
    va_end(args);
    return text;
}

size_t vela_size_add_at(size_t a, size_t b, const char *file, int line)
{
    size_t sum;
    if (__builtin_add_overflow(a, b, &sum)) {
        size_overflow(a, " + ", b, file, line);
    }
    return sum;
}

size_t vela_size_mul_at(size_t a, size_t b, const char *file, int line)
{
    size_t product;
    if (__builtin_mul_overflow(a, b, &product)) {
        vela_oom(a, b, file, line);
    }
    return product;
}
