// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_UTIL_H
#define VELA_UTIL_H

// Small helpers with no dependencies, used across the compositor: time in
// nanoseconds and environment variables.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define VELA_NS_PER_MS INT64_C(1000000)
#define VELA_NS_PER_SEC INT64_C(1000000000)

int64_t vela_timespec_ns(const struct timespec *t);
// CLOCK_MONOTONIC, in nanoseconds.
int64_t vela_now_ns(void);
struct timespec vela_ns_timespec(int64_t ns);

// Region operations (pixman) that failed, out of memory, since the process
// started. Pixman then turns the destination into an empty "broken" region,
// and every later operation that reads it fails too: damage that went
// through a failed operation is unknown, so the frame path compares this
// counter before and after its work and redraws whole output buffers
// instead (docs/memory-allocation-policy.md, "Design for (4)").
extern unsigned long vela_region_failures;
// Wraps a pixman region operation's pixman_bool_t result.
static inline bool vela_region_ok(int ok)
{
    if (!ok) {
        ++vela_region_failures;
    }
    return ok != 0;
}

// The variable is set, not empty and not "0" (VELA_STATS=1).
bool vela_env_flag(const char *name);
// The variable is exactly "0": turns off something that is usually on
// (VELA_SCANOUT=0).
bool vela_env_off(const char *name);
// The variable starts with "1" (the VELA_DEBUG_*=1 diagnostics).
bool vela_env_one(const char *name);
// An integer, or `fallback` if the variable is missing or empty.
int vela_env_int(const char *name, int fallback);

// snprintf that tells whether the text fits: false if it was cut (a cut path
// must not be used).
bool vela_format(char *out, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));

// An allocated array with room for at least `needed` elements of `size`
// bytes: when it's too small, the capacity doubles (realloc). Usage:
//   list = vela_grow(list, &capacity, count + 1, sizeof(*list));
void *vela_grow(void *items, int *capacity, int needed, size_t size);

// The desktop names of XDG_CURRENT_DESKTOP separated by ':' as apps expect
// ("Vela;KDE;" becomes "Vela:KDE": Plasma Login copies DesktopNames from the
// .desktop file as it is). false if it doesn't fit in `size`.
bool vela_desktop_names(const char *current, char *out, size_t size);

static inline int vela_min(int a, int b) { return a < b ? a : b; }
static inline int vela_max(int a, int b) { return a > b ? a : b; }
static inline int vela_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline double vela_clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float vela_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

#endif
