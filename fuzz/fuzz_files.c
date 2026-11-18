// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fuzz.h"

#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char home[PATH_MAX];
static char directory[PATH_MAX];
static char path[PATH_MAX];

static void remove_fixture(void)
{
    unlink(path);
    rmdir(directory);
    rmdir(home);
}

static void prepare_fixture(const char *name)
{
    // Like the compositor tests, use the runner's working directory for scratch files.
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        err(EXIT_FAILURE, "get the fuzz runner's working directory");
    }
    int written = snprintf(home, sizeof(home), "%s/vela-fuzz-XXXXXX", cwd);
    if (written < 0 || (size_t)written >= sizeof(home)) {
        errx(EXIT_FAILURE, "fuzz fixture directory path is too long");
    }
    if (!mkdtemp(home)) {
        err(EXIT_FAILURE, "create the private fuzz fixture directory");
    }
    if (atexit(remove_fixture) != 0) {
        rmdir(home);
        errx(EXIT_FAILURE, "register fuzz fixture cleanup");
    }
    written = snprintf(directory, sizeof(directory), "%s/vela", home);
    if (written < 0 || (size_t)written >= sizeof(directory)) {
        directory[0] = '\0';
        errx(EXIT_FAILURE, "fuzz config directory path is too long");
    }
    written = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        path[0] = '\0';
        errx(EXIT_FAILURE, "fuzz input file path is too long");
    }
    if (mkdir(directory, 0700) != 0 || setenv("XDG_CONFIG_HOME", home, 1) != 0) {
        err(EXIT_FAILURE, "set up the fuzz config directory");
    }
}

const char *vela_fuzz_config_file(const char *name, const uint8_t *data, size_t size)
{
    if (!path[0]) {
        prepare_fixture(name);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        err(EXIT_FAILURE, "open the fuzz input file");
    }
    FILE *file = fdopen(fd, "w");
    if (!file) {
        err(EXIT_FAILURE, "open a stream for the fuzz input file");
    }
    if (fwrite(data, 1, size, file) != size || fclose(file) != 0) {
        err(EXIT_FAILURE, "write the fuzz input file");
    }
    return path;
}
