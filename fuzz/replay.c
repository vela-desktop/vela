// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Runs a fuzz target without libFuzzer, for ctest and any compiler: every
// file given (or every file in a directory given), then --random N inputs
// from a fixed seed, mostly bytes the parsers care about. A crash or a
// sanitizer report fails the test; a real fuzzing run uses libFuzzer
// (fuzz/build.sh, .github/workflows/fuzz.yml).

#include "fuzz.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int run_file(const char *path)
{
    FILE *file = fopen(path, "rbe");
    if (!file) {
        fprintf(stderr, "replay: cannot open %s\n", path);
        return 1;
    }
    uint8_t *data = NULL;
    size_t size = 0;
    size_t capacity = 0;
    for (;;) {
        if (size == capacity) {
            capacity = capacity ? capacity * 2 : 4096;
            uint8_t *grown = realloc(data, capacity);
            if (!grown) {
                free(data);
                fclose(file);
                return 1;
            }
            data = grown;
        }
        size_t got = fread(data + size, 1, capacity - size, file);
        size += got;
        if (got == 0) {
            break;
        }
    }
    fclose(file);
    LLVMFuzzerTestOneInput(data, size);
    free(data);
    return 0;
}

static int run_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "replay: %s not found\n", path);
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        return run_file(path);
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return 1;
    }
    int failures = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        failures += run_file(child);
    }
    closedir(dir);
    return failures;
}

static uint64_t next(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *state = x;
}

static void run_random(long count)
{
    static const char interesting[] = "=[]#\n\r\t \\\"'%$;:.-_/0123456789aAzZ\x6e\x6f\x51\x02\x10";
    uint64_t state = 0x9e3779b97f4a7c15ULL;
    uint8_t buffer[1024];
    for (long i = 0; i < count; ++i) {
        size_t size = next(&state) % sizeof(buffer);
        for (size_t j = 0; j < size; ++j) {
            uint64_t r = next(&state);
            buffer[j] = (r & 3) ? (uint8_t)interesting[(r >> 8) % (sizeof(interesting) - 1)] : (uint8_t)(r >> 16);
        }
        LLVMFuzzerTestOneInput(buffer, size);
    }
}

int main(int argc, char **argv)
{
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--random") == 0 && i + 1 < argc) {
            run_random(strtol(argv[++i], NULL, 10));
        } else {
            failures += run_path(argv[i]);
        }
    }
    return failures ? 1 : 0;
}
