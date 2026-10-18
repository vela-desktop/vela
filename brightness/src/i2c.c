// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "i2c.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

// "i2c-" followed by digits only.
static bool is_bus_name(const char *name)
{
    if (strncmp(name, "i2c-", 4) != 0 || name[4] == '\0') {
        return false;
    }
    for (const char *c = name + 4; *c; ++c) {
        if (*c < '0' || *c > '9') {
            return false;
        }
    }
    return true;
}

static bool is_connector_name(const char *name)
{
    return name[0] != '\0' && strcmp(name, ".") != 0 && strcmp(name, "..") != 0
        && strchr(name, '/') == NULL;
}

int vela_i2c_find_bus(const char *sysfs, const char *connector, char *bus, size_t size,
    const char **how)
{
    if (!is_connector_name(connector)) {
        return -EINVAL;
    }
    char dir[512];
    if (snprintf(dir, sizeof(dir), "%s/class/drm/%s", sysfs, connector) >= (int)sizeof(dir)) {
        return -EINVAL;
    }
    DIR *entries = opendir(dir);
    if (!entries) {
        return -ENOENT;
    }
    // The lowest i2c-N: there is one, but readdir's order is not defined.
    long found = -1;
    struct dirent *entry;
    while ((entry = readdir(entries))) {
        if (is_bus_name(entry->d_name)) {
            long n = strtol(entry->d_name + 4, NULL, 10);
            if (found < 0 || n < found) {
                found = n;
            }
        }
    }
    closedir(entries);
    if (found >= 0) {
        snprintf(bus, size, "i2c-%ld", found);
        if (how) {
            *how = "aux";
        }
        return 0;
    }
    char link[sizeof(dir) + 8];
    char target[512];
    snprintf(link, sizeof(link), "%s/ddc", dir);
    ssize_t length = readlink(link, target, sizeof(target) - 1);
    if (length <= 0) {
        return -ENOENT;
    }
    target[length] = '\0';
    const char *name = strrchr(target, '/');
    name = name ? name + 1 : target;
    if (!is_bus_name(name) || strlen(name) >= size) {
        return -ENOENT;
    }
    strcpy(bus, name);
    if (how) {
        *how = "ddc";
    }
    return 0;
}

void vela_i2c_init(struct vela_i2c *i2c, const char *bus)
{
    memset(i2c, 0, sizeof(*i2c));
    snprintf(i2c->path, sizeof(i2c->path), "/dev/%s", bus);
    i2c->fd = -1;
}

static int i2c_open(void *data)
{
    struct vela_i2c *i2c = data;
    i2c->fd = open(i2c->path, O_RDWR | O_CLOEXEC | O_NOCTTY);
    i2c->addressed = false;
    return i2c->fd < 0 ? -errno : 0;
}

static int i2c_lock(void *data)
{
    struct vela_i2c *i2c = data;
    // ddcutil takes the same lock on the same device (§5.2).
    return flock(i2c->fd, LOCK_EX | LOCK_NB) < 0 ? -errno : 0;
}

static int address(struct vela_i2c *i2c)
{
    if (i2c->addressed) {
        return 0;
    }
    // A kernel driver bound to 0x37 (the out-of-tree ddcci module) makes
    // I2C_SLAVE fail with EBUSY; ddcutil forces it then too.
    if (ioctl(i2c->fd, I2C_SLAVE, VELA_DDC_ADDRESS) < 0
        && (errno != EBUSY || ioctl(i2c->fd, I2C_SLAVE_FORCE, VELA_DDC_ADDRESS) < 0)) {
        return -errno;
    }
    i2c->addressed = true;
    return 0;
}

// One message with I2C_RDWR; the address travels with it, so a driver bound
// to 0x37 can't make the device busy. EINVAL: the driver refuses the
// interface, and read()/write() are used from then on.
static int transfer(struct vela_i2c *i2c, uint8_t *bytes, size_t length, bool reading)
{
    if (!i2c->plain) {
        struct i2c_msg message = {
            .addr = VELA_DDC_ADDRESS,
            .flags = reading ? I2C_M_RD : 0,
            .len = (uint16_t)length,
            .buf = bytes,
        };
        struct i2c_rdwr_ioctl_data data = { .msgs = &message, .nmsgs = 1 };
        if (ioctl(i2c->fd, I2C_RDWR, &data) >= 0) {
            return 0;
        }
        if (errno != EINVAL) {
            return -errno;
        }
        i2c->plain = true;
    }
    int error = address(i2c);
    if (error < 0) {
        return error;
    }
    ssize_t done = reading ? read(i2c->fd, bytes, length) : write(i2c->fd, bytes, length);
    if (done < 0) {
        return -errno;
    }
    return (size_t)done == length ? 0 : -EIO;
}

static int i2c_write(void *data, const uint8_t *bytes, size_t length)
{
    uint8_t copy[16];
    if (length > sizeof(copy)) {
        return -EINVAL;
    }
    memcpy(copy, bytes, length);
    return transfer(data, copy, length, false);
}

static int i2c_read(void *data, uint8_t *bytes, size_t length)
{
    return transfer(data, bytes, length, true);
}

static void i2c_close(void *data)
{
    struct vela_i2c *i2c = data;
    if (i2c->fd >= 0) {
        close(i2c->fd); // the lock goes with it
    }
    i2c->fd = -1;
}

static const char *i2c_mode(void *data)
{
    struct vela_i2c *i2c = data;
    return i2c->plain ? "read/write" : "ioctl";
}

static const struct vela_transport_ops i2c_ops = {
    .open = i2c_open,
    .lock = i2c_lock,
    .write = i2c_write,
    .read = i2c_read,
    .close = i2c_close,
    .mode = i2c_mode,
};

struct vela_transport vela_i2c_transport(struct vela_i2c *i2c)
{
    return (struct vela_transport) { .ops = &i2c_ops, .data = i2c };
}
