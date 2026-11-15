// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the actual private index.theme reader without loading a renderer.
#include "../src/icons.c"
#include <signal.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAILED: %s\n", #expr); exit(1); } } while (0)

int main(void)
{
    char directory[] = "/tmp/vela-icon-metadata-XXXXXX";
    CHECK(mkdtemp(directory));
    char index[PATH_MAX], alias[PATH_MAX];
    snprintf(index, sizeof(index), "%s/index.theme", directory);
    snprintf(alias, sizeof(alias), "%s/alias.theme", directory);
    FILE *file = fopen(index, "w");
    CHECK(file);
    CHECK(fputs("[Icon Theme]\nDirectories=16x16/apps\n[16x16/apps]\nSize=16\nType=Fixed\n", file) >= 0);
    CHECK(fclose(file) == 0);
    CHECK(symlink("index.theme", alias) == 0);
    struct theme_dir *dirs = NULL;
    CHECK(theme_dirs(alias, &dirs) == 1);
    CHECK(strcmp(dirs[0].name, "16x16/apps") == 0 && dirs[0].size == 16);
    free(dirs);
    CHECK(unlink(index) == 0);
    CHECK(mkfifo(index, 0600) == 0);
    alarm(3); // an unchecked fopen would block forever with no writer
    CHECK(theme_dirs(index, &dirs) == 0 && dirs == NULL);
    alarm(0);
    CHECK(unlink(index) == 0);
    CHECK(unlink(alias) == 0);
    CHECK(rmdir(directory) == 0);
    return 0;
}
