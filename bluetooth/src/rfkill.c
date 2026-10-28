// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rfkill.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/rfkill.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *vela_bt_hci_name(const char *path)
{
    static const char prefix[] = "/org/bluez/hci";
    if (!path || strncmp(path, prefix, strlen(prefix)) != 0) {
        return NULL;
    }
    const char *digits = path + strlen(prefix);
    if (!*digits || strlen(digits) > 5 || strspn(digits, "0123456789") != strlen(digits)) {
        return NULL;
    }
    return path + strlen("/org/bluez/");
}

// -errno after a failed call; -EIO if it left errno unset.
static int failure(void)
{
    return errno ? -errno : -EIO;
}

// The first line of a sysfs file, without the newline. 0, or a negative errno.
static int read_text(const char *dir, const char *name, char *text, size_t size)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
        return -ENAMETOOLONG;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return failure();
    }
    ssize_t length = read(fd, text, size - 1);
    int error = length < 0 ? failure() : 0;
    close(fd);
    if (error) {
        return error;
    }
    text[length] = '\0';
    text[strcspn(text, "\n")] = '\0';
    return 0;
}

static int read_number(const char *dir, const char *name)
{
    char text[32];
    if (read_text(dir, name, text, sizeof(text)) < 0) {
        return -1;
    }
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    return end == text || *end || errno || value < 0 || value > INT_MAX ? -1 : (int)value;
}

// The switch described by directory `dir`. 0, or -EINVAL if it's unreadable.
static int read_switch(const char *dir, struct vela_bt_switch *out)
{
    int index = read_number(dir, "index");
    int soft = read_number(dir, "soft");
    int hard = read_number(dir, "hard");
    if (index < 0 || soft < 0 || hard < 0) {
        return -EINVAL;
    }
    *out = (struct vela_bt_switch) { .index = index, .soft = soft != 0, .hard = hard != 0 };
    return 0;
}

static bool is_switch_name(const char *name)
{
    return strncmp(name, "rfkill", 6) == 0 && name[6] && strspn(name + 6, "0123456789") == strlen(name + 6);
}

int vela_bt_adapter_switch(const char *sysfs, const char *hci, struct vela_bt_switch *out)
{
    char dir[PATH_MAX];
    if (strchr(hci, '/') || snprintf(dir, sizeof(dir), "%s/class/bluetooth/%s", sysfs, hci) >= (int)sizeof(dir)) {
        return -EINVAL;
    }
    DIR *entries = opendir(dir);
    if (!entries) {
        return failure();
    }
    int result = -ENOENT;
    struct dirent *entry;
    while ((entry = readdir(entries))) {
        char path[PATH_MAX];
        if (is_switch_name(entry->d_name) && snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) < (int)sizeof(path)) {
            result = read_switch(path, out);
            break;
        }
    }
    closedir(entries);
    return result;
}

// Whether switch `index` belongs to an adapter.
static bool is_adapter_switch(const char *sysfs, int index)
{
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/class/bluetooth", sysfs) >= (int)sizeof(dir)) {
        return false;
    }
    DIR *adapters = opendir(dir);
    if (!adapters) {
        return false;
    }
    bool found = false;
    while (!found) {
        struct dirent *entry = readdir(adapters);
        if (!entry) {
            break;
        }
        struct vela_bt_switch adapter = { .index = -1 };
        if (entry->d_name[0] != '.' && vela_bt_adapter_switch(sysfs, entry->d_name, &adapter) == 0) {
            found = adapter.index == index;
        }
    }
    closedir(adapters);
    return found;
}

int vela_bt_platform_switches(const char *sysfs, struct vela_bt_switch *out, int max)
{
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/class/rfkill", sysfs) >= (int)sizeof(dir)) {
        return 0;
    }
    DIR *entries = opendir(dir);
    if (!entries) {
        return 0;
    }
    int count = 0;
    while (count < max) {
        struct dirent *entry = readdir(entries);
        if (!entry) {
            break;
        }
        char path[PATH_MAX];
        char type[32];
        struct vela_bt_switch found;
        if (is_switch_name(entry->d_name) && snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) < (int)sizeof(path)
            && read_text(path, "type", type, sizeof(type)) == 0 && strcmp(type, "bluetooth") == 0
            && read_switch(path, &found) == 0 && !is_adapter_switch(sysfs, found.index)) {
            out[count++] = found;
        }
    }
    closedir(entries);
    return count;
}

int vela_bt_unblock(const char *device, int index)
{
    int fd = open(device, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    struct rfkill_event event = {
        .idx = (__u32)index,
        .type = RFKILL_TYPE_BLUETOOTH,
        .op = RFKILL_OP_CHANGE,
        .soft = 0,
    };
    ssize_t written = write(fd, &event, RFKILL_EVENT_SIZE_V1);
    int error = written < 0 ? -errno : written == RFKILL_EVENT_SIZE_V1 ? 0 : -EIO;
    close(fd);
    return error;
}
