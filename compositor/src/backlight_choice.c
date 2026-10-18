// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "backlight_choice.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// "card1-eDP-1" → "eDP-1"; a name without a card stays as it is.
static const char *without_card(const char *connector)
{
    if (strncmp(connector, "card", 4) == 0) {
        const char *c = connector + 4;
        while (*c >= '0' && *c <= '9') {
            ++c;
        }
        if (c > connector + 4 && *c == '-') {
            return c + 1;
        }
    }
    return connector;
}

bool vela_connector_is_internal(const char *connector)
{
    const char *name = without_card(connector);
    return strncmp(name, "eDP-", 4) == 0 || strncmp(name, "LVDS-", 5) == 0 || strncmp(name, "DSI-", 4) == 0;
}

static bool resolve(char *out, const char *format, const char *a, const char *b)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), format, a, b) >= (int)sizeof(path)) {
        return false;
    }
    return realpath(path, out) != NULL;
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// firmware first, raw last; an unknown type after them.
static int type_rank(const char *sysfs, const char *device)
{
    static const char *const order[] = { "firmware", "platform", "raw" };
    char path[PATH_MAX];
    char type[32] = "";
    if (snprintf(path, sizeof(path), "%s/class/backlight/%s/type", sysfs, device) < (int)sizeof(path)) {
        FILE *file = fopen(path, "re");
        if (file) {
            if (!fgets(type, sizeof(type), file)) {
                type[0] = '\0';
            }
            fclose(file);
        }
    }
    type[strcspn(type, "\n")] = '\0';
    for (int i = 0; i < 3; ++i) {
        if (strcmp(type, order[i]) == 0) {
            return i;
        }
    }
    return 3;
}

bool vela_backlight_choose(const char *sysfs, const char *connector, char *device, size_t size)
{
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/class/backlight", sysfs) >= (int)sizeof(dir)) {
        return false;
    }
    DIR *entries = opendir(dir);
    if (!entries) {
        return false;
    }
    bool internal = vela_connector_is_internal(connector);
    // The card, and its device (where amdgpu's backlight hangs).
    char card[PATH_MAX] = "";
    char card_device[PATH_MAX] = "";
    size_t card_length = (size_t)(without_card(connector) - connector);
    if (card_length > 1) {
        char name[64];
        snprintf(name, sizeof(name), "%.*s", (int)(card_length - 1), connector);
        if (!resolve(card, "%s/class/drm/%s", sysfs, name)) {
            card[0] = '\0';
        }
        if (!resolve(card_device, "%s/class/drm/%s/device", sysfs, name)) {
            card_device[0] = '\0';
        }
    }
    // The best so far: rank 0 the connector's own, then card + type, then type.
    int best_rank = INT_MAX;
    char best[NAME_MAX + 1] = "";
    struct dirent *entry;
    while ((entry = readdir(entries))) {
        const char *name = entry->d_name;
        if (name[0] == '.') {
            continue;
        }
        char parent[PATH_MAX];
        bool has_parent = resolve(parent, "%s/class/backlight/%s/device", sysfs, name);
        int rank;
        if (has_parent && strcmp(basename_of(parent), connector) == 0) {
            rank = 0;
        } else if (!internal) {
            continue;
        } else {
            bool on_card = has_parent && ((card[0] && strcmp(parent, card) == 0)
                                             || (card_device[0] && strcmp(parent, card_device) == 0));
            rank = 1 + (on_card ? 0 : 4) + type_rank(sysfs, name);
        }
        if (rank < best_rank || (rank == best_rank && strcmp(name, best) < 0)) {
            best_rank = rank;
            snprintf(best, sizeof(best), "%s", name);
        }
    }
    closedir(entries);
    if (best_rank == INT_MAX || strlen(best) >= size) {
        return false;
    }
    snprintf(device, size, "%s", best);
    return true;
}
