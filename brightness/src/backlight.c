// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "backlight.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef VELA_HAVE_SD_BUS
#include <systemd/sd-bus.h>
#endif

// A whole number in a sysfs file. 0, or a negative errno (-ENOENT: missing).
static int read_number(const char *dir, const char *name, uint32_t *out)
{
    char path[PATH_MAX + 32];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
        return -ENAMETOOLONG;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    char text[32];
    ssize_t length = read(fd, text, sizeof(text) - 1);
    int error = length < 0 ? -errno : 0;
    close(fd);
    if (error) {
        return error;
    }
    text[length] = '\0';
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (end == text || (*end != '\0' && *end != '\n') || errno || n > UINT32_MAX) {
        return -EINVAL;
    }
    *out = (uint32_t)n;
    return 0;
}

static enum vela_result result_of(int error)
{
    if (error == -ENOENT || error == -ENODEV || error == -ENXIO) {
        return VELA_RESULT_NO_BUS;
    }
    if (error == -EACCES || error == -EPERM) {
        return VELA_RESULT_NO_ACCESS;
    }
    return VELA_RESULT_NO_ANSWER;
}

int vela_backlight_init(struct vela_backlight *backlight, const char *sysfs, const char *device, bool fake_tree,
    struct vela_clock *clock)
{
    memset(backlight, 0, sizeof(*backlight));
    if (!device[0] || strchr(device, '/') || strcmp(device, ".") == 0 || strcmp(device, "..") == 0
        || strlen(device) >= sizeof(backlight->device)
        || snprintf(backlight->dir, sizeof(backlight->dir), "%s/class/backlight/%s", sysfs, device)
            >= (int)sizeof(backlight->dir)) {
        return -EINVAL;
    }
    snprintf(backlight->device, sizeof(backlight->device), "%s", device);
    backlight->clock = clock;
    backlight->set = fake_tree ? vela_backlight_set_file : vela_backlight_set_logind;
    return 0;
}

enum vela_result vela_backlight_read(struct vela_backlight *backlight, uint32_t *value, uint32_t *max)
{
    struct stat info;
    if (stat(backlight->dir, &info) != 0) {
        return VELA_RESULT_NO_BUS;
    }
    int error = read_number(backlight->dir, "max_brightness", max);
    if (!error) {
        error = read_number(backlight->dir, "brightness", value);
    }
    if (error) {
        return result_of(error);
    }
    if (*max == 0) {
        return VELA_RESULT_UNSUPPORTED;
    }
    if (*value > *max) {
        *value = *max;
    }
    return VELA_RESULT_OK;
}

enum vela_result vela_backlight_write(struct vela_backlight *backlight, uint32_t value, uint32_t *applied)
{
    *applied = value;
    uint32_t max = 0;
    int error = read_number(backlight->dir, "max_brightness", &max);
    if (error) {
        return result_of(error);
    }
    if (max == 0) {
        return VELA_RESULT_UNSUPPORTED;
    }
    uint32_t minimum = max > VELA_BACKLIGHT_COARSE ? 1 : 0;
    value = value < minimum ? minimum : value > max ? max : value;
    error = backlight->set(backlight->set_data, backlight->device, backlight->dir, value);
    if (error) {
        return result_of(error);
    }
    // Some OLED panels show nothing at a low value: actual_brightness says 0.
    for (int raise = 0; value > 0 && value < max && (uint64_t)value * 20 <= max && raise < VELA_BACKLIGHT_OLED_RAISES;
         ++raise) {
        backlight->clock->sleep(backlight->clock->data, VELA_BACKLIGHT_OLED_WAIT_MS);
        uint32_t actual = 0;
        if (read_number(backlight->dir, "actual_brightness", &actual) != 0 || actual != 0) {
            break;
        }
        ++value;
        error = backlight->set(backlight->set_data, backlight->device, backlight->dir, value);
        if (error) {
            return result_of(error);
        }
    }
    *applied = value;
    return VELA_RESULT_OK;
}

static enum vela_result backend_read(void *data, uint32_t *value, uint32_t *max)
{
    return vela_backlight_read(data, value, max);
}

static enum vela_result backend_write(void *data, uint32_t value, uint32_t *applied)
{
    return vela_backlight_write(data, value, applied);
}

struct vela_backend vela_backlight_backend(struct vela_backlight *backlight)
{
    return (struct vela_backend) { .read = backend_read, .write = backend_write, .data = backlight };
}

int vela_backlight_set_file(void *data, const char *device, const char *dir, uint32_t value)
{
    char path[PATH_MAX + 32];
    if (snprintf(path, sizeof(path), "%s/brightness", dir) >= (int)sizeof(path)) {
        return -ENAMETOOLONG;
    }
    FILE *file = fopen(path, "we");
    if (!file) {
        return -errno;
    }
    int written = fprintf(file, "%u\n", value);
    int closed = fclose(file);
    return written < 0 || closed != 0 ? -EIO : 0;
}

#ifdef VELA_HAVE_SD_BUS

#define LOGIND_TIMEOUT_US (2 * 1000 * 1000) // well within the compositor's watchdog

int vela_backlight_set_logind(void *data, const char *device, const char *dir, uint32_t value)
{
    // One connection for the process's life: a drag is many calls. Dropped
    // on an error, made again on the next call.
    static sd_bus *bus;
    if (!bus) {
        int error = sd_bus_open_system(&bus);
        if (error < 0) {
            bus = NULL;
            return error;
        }
        sd_bus_set_method_call_timeout(bus, LOGIND_TIMEOUT_US);
    }
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int result = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
        "org.freedesktop.login1.Session", "SetBrightness", &error, NULL, "ssu", "backlight", device, value);
    if (result < 0) {
        fprintf(stderr, "vela-brightness: SetBrightness %s %u: %s\n", device, value,
            error.message ? error.message : strerror(-result));
        if (sd_bus_error_has_name(&error, SD_BUS_ERROR_ACCESS_DENIED)) {
            result = -EACCES;
        } else if (!sd_bus_error_is_set(&error)) {
            bus = sd_bus_flush_close_unref(bus); // the connection itself failed
        }
    }
    sd_bus_error_free(&error);
    return result < 0 ? result : 0;
}

#else

int vela_backlight_set_logind(void *data, const char *device, const char *dir, uint32_t value)
{
    return -EACCES; // built without sd-bus: Vela can't reach logind
}

#endif
