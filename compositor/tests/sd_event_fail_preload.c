// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// LD_PRELOAD shim for the daemons' tests: makes chosen sd-event source
// creations fail as they would with descriptors exhausted, to reach their
// callers' local paths (docs/memory-allocation-policy.md, point 2b).
//
// VELA_TEST_SD_EVENT_FAIL lists what fails, separated by spaces:
// "defer", "child", "time_after=<usec>" (sd_event_add_time for a time at
// least that far ahead), and "time_relative=<usec>"
// (sd_event_add_time_relative with that delay). The values pick one timer,
// so the others of the same clock still work. The failures return -EMFILE.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

// Whether `word` is listed: alone when `value` is NULL, or as
// "<word>=<n>" with n == *value (or n <= *value when `at_least`).
static bool listed_as(const char *word, const uint64_t *value, bool at_least)
{
    const char *list = getenv("VELA_TEST_SD_EVENT_FAIL");
    size_t length = strlen(word);
    for (const char *at = list; at && *at;) {
        while (*at == ' ') {
            ++at;
        }
        const char *end = strchr(at, ' ');
        size_t item = end ? (size_t)(end - at) : strlen(at);
        if (item >= length && strncmp(at, word, length) == 0) {
            if (!value && item == length) {
                return true;
            }
            uint64_t listed_value = 0;
            if (value && at[length] == '=' && sscanf(at + length + 1, "%" SCNu64, &listed_value) == 1
                && (at_least ? listed_value <= *value : listed_value == *value)) {
                return true;
            }
        }
        at = end;
    }
    return false;
}

static bool listed(const char *word, const uint64_t *value)
{
    return listed_as(word, value, false);
}

#define REAL(name) ((__typeof__(&name))dlsym(RTLD_NEXT, #name))

int sd_event_add_defer(void *e, void **s, void *callback, void *userdata);
int sd_event_add_defer(void *e, void **s, void *callback, void *userdata)
{
    return listed("defer", NULL) ? -EMFILE : REAL(sd_event_add_defer)(e, s, callback, userdata);
}

int sd_event_add_child(void *e, void **s, pid_t pid, int options, void *callback, void *userdata);
int sd_event_add_child(void *e, void **s, pid_t pid, int options, void *callback, void *userdata)
{
    return listed("child", NULL) ? -EMFILE : REAL(sd_event_add_child)(e, s, pid, options, callback, userdata);
}

int sd_event_add_time(void *e, void **s, clockid_t clock, uint64_t usec, uint64_t accuracy, void *callback,
    void *userdata);
int sd_event_add_time(void *e, void **s, clockid_t clock, uint64_t usec, uint64_t accuracy, void *callback,
    void *userdata)
{
    struct timespec now;
    uint64_t ahead = 0;
    if (clock_gettime(clock, &now) == 0) {
        uint64_t now_us = (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
        ahead = usec > now_us ? usec - now_us : 0;
    }
    return listed_as("time_after", &ahead, true)
        ? -EMFILE
        : REAL(sd_event_add_time)(e, s, clock, usec, accuracy, callback, userdata);
}

int sd_event_add_time_relative(void *e, void **s, clockid_t clock, uint64_t usec, uint64_t accuracy,
    void *callback, void *userdata);
int sd_event_add_time_relative(void *e, void **s, clockid_t clock, uint64_t usec, uint64_t accuracy,
    void *callback, void *userdata)
{
    return listed("time_relative", &usec)
        ? -EMFILE
        : REAL(sd_event_add_time_relative)(e, s, clock, usec, accuracy, callback, userdata);
}
