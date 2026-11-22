// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lock_keys.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

enum vela_numlock vela_numlock_parse(const char *value)
{
    if (value && strcmp(value, "on") == 0) {
        return VELA_NUMLOCK_ON;
    }
    if (value && strcmp(value, "last") == 0) {
        return VELA_NUMLOCK_LAST;
    }
    return VELA_NUMLOCK_OFF;
}

// The bit of modifier `name` in a serialized mask, 0 if the keymap has none.
static uint32_t bit(struct xkb_keymap *keymap, const char *name)
{
    const xkb_mod_index_t index = keymap ? xkb_keymap_mod_get_index(keymap, name) : XKB_MOD_INVALID;
    return index == XKB_MOD_INVALID || index >= 32 ? 0 : (uint32_t)1 << index;
}

void vela_locks_read(struct xkb_keymap *keymap, uint32_t locked, bool *caps, bool *num)
{
    const uint32_t caps_bit = bit(keymap, XKB_MOD_NAME_CAPS);
    const uint32_t num_bit = bit(keymap, XKB_MOD_NAME_NUM);
    *caps = caps_bit && (locked & caps_bit);
    *num = num_bit && (locked & num_bit);
}

uint32_t vela_locks_apply(struct xkb_keymap *keymap, uint32_t locked, bool caps, bool num)
{
    const uint32_t caps_bit = bit(keymap, XKB_MOD_NAME_CAPS);
    const uint32_t num_bit = bit(keymap, XKB_MOD_NAME_NUM);
    locked = caps ? locked | caps_bit : locked & ~caps_bit;
    return num ? locked | num_bit : locked & ~num_bit;
}

bool vela_numlock_path(char *out, size_t size)
{
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    int n;
    // XDG wants absolute base directories: a relative one is ignored.
    if (state && *state == '/') {
        n = snprintf(out, size, "%s/vela/numlock", state);
    } else if (home && *home == '/') {
        n = snprintf(out, size, "%s/.local/state/vela/numlock", home);
    } else {
        return false;
    }
    return n > 0 && (size_t)n < size;
}

bool vela_numlock_load(const char *path)
{
    FILE *file = path ? fopen(path, "re") : NULL;
    if (!file) {
        return false;
    }
    // Exactly what vela_numlock_save() writes: anything longer is damaged.
    char text[8] = "";
    const size_t read = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    return read == 3 && memcmp(text, "on\n", 3) == 0;
}

// Creates `dir` and its parents, private to the user.
static bool make_dirs(char *dir)
{
    for (char *slash = strchr(dir + 1, '/');; slash = strchr(slash + 1, '/')) {
        if (slash) {
            *slash = '\0';
        }
        const bool made = mkdir(dir, 0700) == 0 || errno == EEXIST;
        if (slash) {
            *slash = '/';
        }
        if (!made) {
            return false;
        }
        if (!slash) {
            return true;
        }
    }
}

bool vela_numlock_save(const char *path, bool on)
{
    char dir[4096];
    char temporary[4096];
    if (!path || snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)
        || snprintf(temporary, sizeof(temporary), "%s.new", path) >= (int)sizeof(temporary)) {
        return false;
    }
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        return false;
    }
    *slash = '\0';
    if (!make_dirs(dir)) {
        return false;
    }
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    const char *text = on ? "on\n" : "off\n";
    const size_t length = strlen(text);
    const bool written = write(fd, text, length) == (ssize_t)length && fsync(fd) == 0;
    if (close(fd) != 0 || !written || rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}

bool vela_numlock_initial(enum vela_numlock mode, const char *path)
{
    switch (mode) {
    case VELA_NUMLOCK_ON: return true;
    case VELA_NUMLOCK_LAST: return vela_numlock_load(path);
    case VELA_NUMLOCK_OFF:
    default: return false;
    }
}
