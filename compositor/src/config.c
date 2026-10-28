// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"

#include "legacy_names.h"
#include "path_security.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool vela_config_path(const char *name, char *out, size_t size)
{
    const char *config = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    // Callers supply a fixed filename (or NULL for the directory). The
    // filename is never allowed to turn into a second filesystem path.
    if (!out || size == 0 || (name && !vela_path_component_safe(name))) {
        return false;
    }
    const char *file = name ? "/" : "";
    name = name ? name : "";
    int n;
    // XDG requires absolute base directories. Ignore a relative override
    // rather than letting it redirect settings beneath the working directory.
    if (config && *config == '/') {
        n = snprintf(out, size, "%s/vela%s%s", config, file, name);
    } else if (home && *home == '/') {
        n = snprintf(out, size, "%s/.config/vela%s%s", home, file, name);
    } else {
        return false;
    }
    return n > 0 && (size_t)n < size;
}

// The line with the current names (legacy_names.h): a new copy if it changed,
// NULL if it's fine as it is. Comments and empty lines stay.
static char *modernize(const char *line)
{
    const char *eq = strchr(line, '=');
    if (!*line || *line == '#' || !eq) {
        return NULL;
    }
    char *key = strndup(line, (size_t)(eq - line));
    const char *value = eq + 1;
    const char *new_key = vela_legacy_key(key);
    const char *new_value = vela_legacy_value(new_key ? new_key : key, value);
    char *result = NULL;
    if (new_key || new_value) {
        if (asprintf(&result, "%s=%s", new_key ? new_key : key, new_value ? new_value : value) < 0) {
            result = NULL;
        }
    }
    free(key);
    return result;
}

// The lines of a file, to rewrite it. Each line is allocated.
struct lines {
    char **items;
    int count;
    int capacity;
};

static void lines_push(struct lines *lines, char *line)
{
    if (lines->count == lines->capacity) {
        lines->capacity = lines->capacity ? lines->capacity * 2 : 32;
        lines->items = realloc(lines->items, (size_t)lines->capacity * sizeof(*lines->items));
    }
    lines->items[lines->count++] = line;
}

static void lines_free(struct lines *lines)
{
    for (int i = 0; i < lines->count; ++i) {
        free(lines->items[i]);
    }
    free(lines->items);
}

// The file line by line, already with the current names; *changed if some
// still had the old ones.
static void read_lines(const char *path, struct lines *lines, bool *changed)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        return;
    }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t n;
    while ((n = getline(&line, &capacity, file)) >= 0) {
        if (n > 0 && line[n - 1] == '\n') {
            line[n - 1] = '\0';
        }
        char *modern = modernize(line);
        if (modern && changed) {
            *changed = true;
        }
        lines_push(lines, modern ? modern : strdup(line));
    }
    free(line);
    fclose(file);
}

// A new file in place of the old one: readers never see half a file.
static bool write_lines(const char *path, const struct lines *lines)
{
    // mkstemp uses 0600 regardless of umask, and prevents a predictable
    // .tmp symlink from redirecting configuration writes.
    char temporary[PATH_MAX + 8];
    int n = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(temporary)) {
        return false;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) {
        return false;
    }
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlink(temporary);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < lines->count; ++i) {
        ok = fprintf(file, "%s\n", lines->items[i]) >= 0 && ok;
    }
    ok = fclose(file) == 0 && ok;
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}

static void set_entry(struct vela_config *config, const char *line, size_t key_length)
{
    char *entry = strdup(line);
    if (!entry) {
        return;
    }
    entry[key_length] = '\0';
    for (int i = 0; i < config->count; ++i) {
        if (strcmp(config->entries[i].key, entry) == 0) {
            free(config->entries[i].key);
            config->entries[i].key = entry;
            config->entries[i].value = entry + key_length + 1;
            return;
        }
    }
    if (config->count == config->capacity) {
        int capacity = config->capacity ? config->capacity * 2 : 32;
        struct vela_config_entry *entries = realloc(config->entries, (size_t)capacity * sizeof(*entries));
        if (!entries) {
            free(entry);
            return;
        }
        config->entries = entries;
        config->capacity = capacity;
    }
    config->entries[config->count].key = entry;
    config->entries[config->count].value = entry + key_length + 1;
    ++config->count;
}

void vela_config_read(struct vela_config *config)
{
    memset(config, 0, sizeof(*config));
    char path[PATH_MAX];
    if (!vela_config_path("vela.conf", path, sizeof(path))) {
        return;
    }
    struct lines lines = { 0 };
    read_lines(path, &lines, NULL);
    for (int i = 0; i < lines.count; ++i) {
        const char *line = lines.items[i];
        const char *eq = strchr(line, '=');
        if (*line && *line != '#' && eq) {
            set_entry(config, line, (size_t)(eq - line));
        }
    }
    lines_free(&lines);
}

void vela_config_finish(struct vela_config *config)
{
    for (int i = 0; i < config->count; ++i) {
        free(config->entries[i].key);
    }
    free(config->entries);
    memset(config, 0, sizeof(*config));
}

const char *vela_config_get(const struct vela_config *config, const char *key, const char *fallback)
{
    for (int i = 0; i < config->count; ++i) {
        if (strcmp(config->entries[i].key, key) == 0) {
            return config->entries[i].value;
        }
    }
    return fallback;
}

bool vela_config_flag(const struct vela_config *config, const char *key, bool fallback)
{
    const char *value = vela_config_get(config, key, NULL);
    if (!value || !*value) {
        return fallback;
    }
    return strcmp(value, "yes") == 0 || strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
}


bool vela_config_write_checked(const char *key, const char *value)
{
    char dir[PATH_MAX];
    char path[PATH_MAX];
    if (!vela_config_path(NULL, dir, sizeof(dir)) || !vela_config_path("vela.conf", path, sizeof(path))) {
        return false;
    }
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
        return false;
    }

    struct lines lines = { 0 };
    read_lines(path, &lines, NULL);
    char *wanted = NULL;
    if (asprintf(&wanted, "%s=%s", key, value) < 0) {
        lines_free(&lines);
        return false;
    }
    size_t key_length = strlen(key);
    bool found = false;
    int kept = 0;
    for (int i = 0; i < lines.count; ++i) {
        char *line = lines.items[i];
        bool same_key = strncmp(line, key, key_length) == 0 && line[key_length] == '=';
        if (same_key && found) {
            free(line); // duplicates: one is left
            continue;
        }
        if (same_key) {
            free(line);
            line = strdup(wanted);
            found = true;
        }
        lines.items[kept++] = line;
    }
    lines.count = kept;
    if (lines.count == 0) {
        lines_push(&lines, strdup("# Vela settings (written by the Settings app)"));
    }
    if (!found) {
        lines_push(&lines, strdup(wanted));
    }
    free(wanted);
    bool ok = write_lines(path, &lines);
    lines_free(&lines);
    return ok;
}

void vela_config_write(const char *key, const char *value)
{
    (void)vela_config_write_checked(key, value);
}

void vela_config_migrate(void)
{
    char path[PATH_MAX];
    if (!vela_config_path("vela.conf", path, sizeof(path))) {
        return;
    }
    struct lines lines = { 0 };
    bool changed = false;
    read_lines(path, &lines, &changed);
    if (changed) {
        write_lines(path, &lines);
    }
    lines_free(&lines);
}
