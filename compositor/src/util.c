// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned long vela_region_failures;

int64_t vela_timespec_ns(const struct timespec *t)
{
    return (int64_t)t->tv_sec * VELA_NS_PER_SEC + t->tv_nsec;
}

int64_t vela_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return vela_timespec_ns(&t);
}

struct timespec vela_ns_timespec(int64_t ns)
{
    struct timespec t = { .tv_sec = (time_t)(ns / VELA_NS_PER_SEC), .tv_nsec = (long)(ns % VELA_NS_PER_SEC) };
    return t;
}

bool vela_env_flag(const char *name)
{
    const char *value = getenv(name);
    return value && *value && strcmp(value, "0") != 0;
}

bool vela_env_off(const char *name)
{
    const char *value = getenv(name);
    return value && strcmp(value, "0") == 0;
}

bool vela_env_one(const char *name)
{
    const char *value = getenv(name);
    return value && *value == '1';
}

int vela_env_int(const char *name, int fallback)
{
    const char *value = getenv(name);
    return value && *value ? atoi(value) : fallback;
}

bool vela_format(char *out, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out, size, format, args);
    va_end(args);
    return n >= 0 && (size_t)n < size;
}

bool vela_desktop_names(const char *current, char *out, size_t size)
{
    size_t length = 0;
    out[0] = '\0';
    for (const char *name = current; name && *name;) {
        size_t n = strcspn(name, ";:");
        if (n > 0) {
            int written = snprintf(out + length, size - length, "%s%.*s", length ? ":" : "", (int)n, name);
            if (written < 0 || (size_t)written >= size - length) {
                return false;
            }
            length += (size_t)written;
        }
        name += n;
        name += *name ? 1 : 0;
    }
    return true;
}
