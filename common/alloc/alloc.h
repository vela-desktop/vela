// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_ALLOC_H
#define VELA_ALLOC_H

// The host allocation helpers (docs/memory-allocation-policy.md, "Policy").
// They never return NULL: a failed allocation, or a size whose arithmetic
// overflows, writes one line to stderr and aborts, and the process's
// supervisor restarts it. Every block comes back zeroed.
//
// External sizes must be checked against their documented bound where they
// enter, before they reach these helpers: an abort that another party can
// trigger at will would end the user's session.
//
// The public names are macros that pass the caller's location, so the line
// in vela.log names the call site, not this file.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VELA_ALLOC_NORETURN __attribute__((noreturn))

// Internal entry points. Every failure reaches vela_oom with these file/line.
void *vela_alloc_at(size_t count, size_t size, const char *file, int line);
void *vela_grow_size_at(void *items, size_t *capacity, size_t needed, size_t size,
    const char *file, int line);
// `needed` is wider than the int capacity on purpose: a caller's size_t or
// int64_t count arrives intact and is range-checked here, not truncated by an
// implicit conversion at the call.
void *vela_grow_int_at(void *items, int *capacity, long long needed, size_t size,
    const char *file, int line);
char *vela_strdup_at(const char *s, const char *file, int line);
char *vela_strndup_at(const char *s, size_t n, const char *file, int line);
char *vela_asprintf_at(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
size_t vela_size_add_at(size_t a, size_t b, const char *file, int line);
size_t vela_size_mul_at(size_t a, size_t b, const char *file, int line);

// The failure paths. They write with write(2) from a stack buffer (no stdio,
// no heap) and then call abort().
// "vela: out of memory (<count> x <size> bytes) at <file>:<line>; rss <n> MiB; available <n> MiB"
VELA_ALLOC_NORETURN void vela_oom(size_t count, size_t size, const char *file, int line);
// A programming error, such as a negative count: "vela: allocation misuse (<what>) at <file>:<line>".
VELA_ALLOC_NORETURN void vela_alloc_misuse(const char *what, const char *file, int line);
// A library call that can only fail for lack of memory returned NULL:
// "vela: out of memory (<expression>) at <file>:<line>; rss <n> MiB; available <n> MiB".
VELA_ALLOC_NORETURN void vela_must_failed(const char *expression, const char *file, int line);
// `value`, or vela_must_failed when it is NULL.
const void *vela_must_at(const void *value, const char *expression, const char *file, int line);

// Public API.
#define vela_alloc(count, size) vela_alloc_at((count), (size), __FILE__, __LINE__)
#define VELA_NEW(type) ((type *)vela_alloc(1, sizeof(type)))
#define VELA_NEW_N(type, n) ((type *)vela_alloc((size_t)(n), sizeof(type)))
#ifndef __cplusplus
// An array with room for at least `needed` elements of `size` bytes: when it
// is missing or too small, the capacity doubles (starting from 8 elements) and
// the new tail is zeroed. One name for both capacity types: size_t for byte
// buffers, int for the element arrays. Usage:
//   list = vela_grow(list, &capacity, count + 1, sizeof(*list));
#define vela_grow(items, capacity, needed, size)                                   \
    _Generic((capacity), size_t *: vela_grow_size_at, int *: vela_grow_int_at)(   \
        (items), (capacity), (needed), (size), __FILE__, __LINE__)
#endif
#define vela_strdup(s) vela_strdup_at((s), __FILE__, __LINE__)
#define vela_strndup(s, n) vela_strndup_at((s), (n), __FILE__, __LINE__)
#define vela_asprintf(...) vela_asprintf_at(__FILE__, __LINE__, __VA_ARGS__)
#define vela_size_add(a, b) vela_size_add_at((a), (b), __FILE__, __LINE__)
#define vela_size_mul(a, b) vela_size_mul_at((a), (b), __FILE__, __LINE__)
#ifndef __cplusplus
// The result of a library call that fails only for lack of memory, such as
// wl_event_loop_add_idle or wl_resource_create, never NULL: a NULL result
// aborts like a failed helper. Only for the calls listed as OOM-only in
// common/alloc/library-results.md; any other failure is handled where it
// happens. The expression is evaluated once and keeps its pointer type.
#define VELA_MUST(expr)                                                                \
    ((__typeof__(expr))vela_must_at((expr), #expr, __FILE__, __LINE__))
#endif

#ifdef __cplusplus
}
#endif

#endif
