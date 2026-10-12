// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "output_config.h"

#include "config.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The keys of a section, in the order they are written. Others are ignored
// (and disappear at the next write).
enum key { ENABLED, MODE, SCALE, ROTATION, POSITION, KEY_COUNT };
static const char *const key_names[KEY_COUNT] = { "enabled", "mode", "scale", "rotation", "position" };

static const char *const transform_names[8] = {
    "normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180", "flipped-270",
};

// A section of the file: one monitor. Values are short by nature
// ("2560x1440@180.000"); a longer one, written by hand, is truncated.
struct section {
    char *name;
    char values[KEY_COUNT][64];
    bool has[KEY_COUNT];
};

// The whole file, in order. It's read, changed, written back.
struct file {
    struct section *sections;
    int count;
    int capacity;
};

static struct section *add_section(struct file *file, const char *name, size_t length)
{
    if (file->count == file->capacity) {
        file->capacity = file->capacity ? file->capacity * 2 : 8;
        file->sections = realloc(file->sections, (size_t)file->capacity * sizeof(*file->sections));
    }
    struct section *section = &file->sections[file->count++];
    memset(section, 0, sizeof(*section));
    section->name = strndup(name, length);
    return section;
}

static struct section *find_section(struct file *file, const char *name)
{
    for (int i = 0; i < file->count; ++i) {
        if (strcmp(file->sections[i].name, name) == 0) {
            return &file->sections[i];
        }
    }
    return NULL;
}

static void free_file(struct file *file)
{
    for (int i = 0; i < file->count; ++i) {
        free(file->sections[i].name);
    }
    free(file->sections);
}

static void set_value(struct section *section, enum key key, const char *value)
{
    snprintf(section->values[key], sizeof(section->values[key]), "%s", value);
    section->has[key] = true;
}

// The Italian keys and values of schermi.conf with the current names.
static const char *modern_key(const char *key)
{
    static const char *const legacy[KEY_COUNT] = { "attivo", "modo", "scala", "rotazione", "posizione" };
    for (int i = 0; i < KEY_COUNT; ++i) {
        if (strcmp(key, legacy[i]) == 0) {
            return key_names[i];
        }
    }
    return key;
}

static void modern_value(const char *value, char *out, size_t size)
{
    if (strcmp(value, "si") == 0 || strcmp(value, "sì") == 0) {
        snprintf(out, size, "yes");
    } else if (strcmp(value, "normale") == 0) {
        snprintf(out, size, "normal");
    } else if (strncmp(value, "specchio", 8) == 0) {
        snprintf(out, size, "flipped%s", value + 8);
    } else {
        snprintf(out, size, "%s", value);
    }
}

static void read_file(struct file *file)
{
    memset(file, 0, sizeof(*file));
    char path[PATH_MAX];
    if (!vela_config_path("outputs.conf", path, sizeof(path))) {
        return;
    }
    FILE *in = fopen(path, "r");
    bool legacy = !in;
    if (legacy && vela_config_path("schermi.conf", path, sizeof(path))) {
        in = fopen(path, "r");
    }
    if (!in) {
        return;
    }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t n;
    struct section *current = NULL;
    while ((n = getline(&line, &capacity, in)) >= 0) {
        if (n > 0 && line[n - 1] == '\n') {
            line[--n] = '\0';
        }
        if (n == 0 || line[0] == '#') {
            continue;
        }
        if (line[0] == '[' && line[n - 1] == ']') {
            current = add_section(file, line + 1, n >= 2 ? (size_t)n - 2 : 0);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!current || !eq) {
            continue;
        }
        *eq = '\0';
        const char *key = legacy ? modern_key(line) : line;
        char value[64];
        if (legacy) {
            modern_value(eq + 1, value, sizeof(value));
        } else {
            snprintf(value, sizeof(value), "%s", eq + 1);
        }
        for (int i = 0; i < KEY_COUNT; ++i) {
            if (strcmp(key, key_names[i]) == 0) {
                set_value(current, (enum key)i, value);
            }
        }
    }
    free(line);
    fclose(in);
}

bool vela_output_config_load(const char *key, struct vela_saved_output *out)
{
    struct file file;
    read_file(&file);
    struct section *section = find_section(&file, key);
    if (!section) {
        free_file(&file);
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->enabled = !section->has[ENABLED] || strcmp(section->values[ENABLED], "no") != 0;
    if (section->has[MODE] && section->values[MODE][0]) {
        double hz = 0.0;
        if (sscanf(section->values[MODE], "%dx%d@%lf", &out->width, &out->height, &hz) >= 2) {
            out->refresh_mhz = (int)(hz * 1000.0 + 0.5);
        }
    }
    if (section->has[SCALE] && section->values[SCALE][0]) {
        out->scale = strtof(section->values[SCALE], NULL);
    }
    if (section->has[ROTATION]) {
        for (int t = 0; t < 8; ++t) {
            if (strcmp(section->values[ROTATION], transform_names[t]) == 0) {
                out->transform = t;
            }
        }
    }
    if (section->has[POSITION] && section->values[POSITION][0]) {
        out->has_position = sscanf(section->values[POSITION], "%d,%d", &out->x, &out->y) == 2;
    }
    free_file(&file);
    return true;
}

bool vela_output_config_save(const struct vela_output_config_entry *entries, int count)
{
    char path[PATH_MAX];
    char dir[PATH_MAX];
    char legacy[PATH_MAX];
    if (!vela_config_path("outputs.conf", path, sizeof(path)) || !vela_config_path(NULL, dir, sizeof(dir))) {
        return false;
    }
    struct file file;
    read_file(&file);
    for (int i = 0; i < count; ++i) {
        const struct vela_output_config_entry *entry = &entries[i];
        struct section *section = find_section(&file, entry->key);
        if (!section) {
            section = add_section(&file, entry->key, strlen(entry->key));
        }
        set_value(section, ENABLED, entry->enabled ? "yes" : "no");
        if (!entry->enabled) {
            continue;
        }
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%dx%d@%.3f", entry->width, entry->height, entry->refresh_mhz / 1000.0);
        set_value(section, MODE, buffer);
        snprintf(buffer, sizeof(buffer), "%g", (double)entry->scale);
        set_value(section, SCALE, buffer);
        int transform = entry->transform >= 0 && entry->transform < 8 ? entry->transform : 0;
        set_value(section, ROTATION, transform_names[transform]);
        snprintf(buffer, sizeof(buffer), "%d,%d", entry->x, entry->y);
        set_value(section, POSITION, buffer);
    }

    bool saved = false;
    // The ~/.config directory, then ~/.config/vela.
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
        *slash = '/';
    }
    mkdir(dir, 0755);
    char temporary[PATH_MAX + 8];
    int length = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    if (length >= 0 && (size_t)length < sizeof(temporary)) {
        int fd = mkstemp(temporary); // mode 0600, no predictable .tmp filename
        if (fd >= 0) {
            FILE *out = fdopen(fd, "w");
            if (!out) {
                close(fd);
                unlink(temporary);
            } else {
                bool ok = fputs("# Vela's outputs, written by Vela when you change the configuration.\n"
                                "# One monitor per section: [make model serial number].\n",
                              out) >= 0;
                for (int i = 0; i < file.count; ++i) {
                    const struct section *section = &file.sections[i];
                    ok = fprintf(out, "\n[%s]\n", section->name) >= 0 && ok;
                    for (int k = 0; k < KEY_COUNT; ++k) {
                        if (section->has[k]) {
                            ok = fprintf(out, "%s=%s\n", key_names[k], section->values[k]) >= 0 && ok;
                        }
                    }
                }
                ok = fclose(out) == 0 && ok;
                if (ok && rename(temporary, path) == 0) {
                    saved = true;
                    if (vela_config_path("schermi.conf", legacy, sizeof(legacy))) {
                        remove(legacy); // outputs.conf exists now
                    }
                } else {
                    unlink(temporary);
                }
            }
        }
    }
    free_file(&file);
    return saved;
}
