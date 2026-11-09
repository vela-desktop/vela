// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// LD_PRELOAD shim for the tests: makes getline fail partway through a chosen
// file, as an I/O error or a failed allocation would.
//
// VELA_TEST_GETLINE_FAIL="<file name> <line> <errno>", such as
// "vela.conf 2 EIO": a stream opened on a path whose last component is
// <file name> returns lines 1..<line>-1 normally, then -1 with errno set to
// EIO or ENOMEM, as many times as it is asked. The variable is read at each
// fopen, so a test can change it between operations; unset, nothing
// changes.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define TRACKED 16

static struct {
    FILE *file;
    long fail_at; // 1-based line that fails
    int error;
    long lines;
} tracked[TRACKED];

static void *next(const char *name)
{
    return dlsym(RTLD_NEXT, name);
}

static void track(FILE *file, const char *path)
{
    const char *spec = getenv("VELA_TEST_GETLINE_FAIL");
    if (!file || !path || !spec || !*spec) {
        return;
    }
    char name[256];
    long line = 0;
    char error[16];
    if (sscanf(spec, "%255s %ld %15s", name, &line, error) != 3 || line < 1) {
        return;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strcmp(base, name) != 0) {
        return;
    }
    for (int i = 0; i < TRACKED; ++i) {
        if (!tracked[i].file) {
            tracked[i].file = file;
            tracked[i].fail_at = line;
            tracked[i].error = strcmp(error, "ENOMEM") == 0 ? ENOMEM : EIO;
            tracked[i].lines = 0;
            return;
        }
    }
}

FILE *fopen(const char *path, const char *mode)
{
    static FILE *(*real)(const char *, const char *);
    if (!real) {
        real = (FILE * (*)(const char *, const char *)) next("fopen");
    }
    FILE *file = real(path, mode);
    track(file, path);
    return file;
}

FILE *fopen64(const char *path, const char *mode)
{
    static FILE *(*real)(const char *, const char *);
    if (!real) {
        real = (FILE * (*)(const char *, const char *)) next("fopen64");
    }
    FILE *file = real(path, mode);
    track(file, path);
    return file;
}

int fclose(FILE *file)
{
    static int (*real)(FILE *);
    if (!real) {
        real = (int (*)(FILE *))next("fclose");
    }
    for (int i = 0; i < TRACKED; ++i) {
        if (tracked[i].file == file) {
            tracked[i].file = NULL;
        }
    }
    return real(file);
}

// True (with errno set) when this read of `file` must fail.
static int injected(FILE *file)
{
    for (int i = 0; i < TRACKED; ++i) {
        if (tracked[i].file == file && ++tracked[i].lines >= tracked[i].fail_at) {
            fprintf(stderr, "getline-fail: injected %s on line %ld\n",
                tracked[i].error == ENOMEM ? "ENOMEM" : "EIO", tracked[i].lines);
            errno = tracked[i].error;
            return 1;
        }
    }
    return 0;
}

typedef ssize_t (*delim_fn)(char **, size_t *, int, FILE *);

// With optimization, glibc's headers turn getline() into __getdelim(): wrap
// every entry point.
ssize_t __getdelim(char **line, size_t *capacity, int delimiter, FILE *file)
{
    static delim_fn real;
    if (!real) {
        real = (delim_fn)next("__getdelim");
    }
    return injected(file) ? -1 : real(line, capacity, delimiter, file);
}

ssize_t getdelim(char **line, size_t *capacity, int delimiter, FILE *file)
{
    static delim_fn real;
    if (!real) {
        real = (delim_fn)next("getdelim");
    }
    return injected(file) ? -1 : real(line, capacity, delimiter, file);
}

ssize_t getline(char **line, size_t *capacity, FILE *file)
{
    static delim_fn real;
    if (!real) {
        real = (delim_fn)next("__getdelim");
    }
    return injected(file) ? -1 : real(line, capacity, '\n', file);
}
