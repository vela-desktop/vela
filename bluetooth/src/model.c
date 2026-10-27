// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// BlueZ's adapters and devices, as the daemon follows them
// (docs/bluetooth.md §6.2).

#include "daemon.h"

#include "describe.h"
#include "log.h"
#include "rfkill.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define ADAPTER1 "org.bluez.Adapter1"
#define DEVICE1 "org.bluez.Device1"
#define BATTERY1 "org.bluez.Battery1"

static bool same(const char *a, const char *b)
{
    return strcmp(a ? a : "", b ? b : "") == 0;
}

// Replaces *text with a copy of `value`. Whether it changed.
static bool set_text(char **text, const char *value)
{
    if (same(*text, value)) {
        return false;
    }
    char *copy = strdup(value ? value : "");
    if (!copy) {
        return false;
    }
    free(*text);
    *text = copy;
    return true;
}

static bool set_bool(bool *field, int value)
{
    bool changed = *field != (value != 0);
    *field = value != 0;
    return changed;
}

// Reads the variant at the message's position if it holds one `type`;
// otherwise skips it. Whether it was read.
static bool read_variant(sd_bus_message *m, char type, void *out)
{
    const char *contents = NULL;
    char kind;
    if (sd_bus_message_peek_type(m, &kind, &contents) <= 0 || kind != 'v' || !contents || contents[0] != type
        || contents[1]) {
        sd_bus_message_skip(m, "v");
        return false;
    }
    if (sd_bus_message_enter_container(m, 'v', contents) < 0) {
        return false;
    }
    bool read = sd_bus_message_read_basic(m, type, out) > 0;
    sd_bus_message_exit_container(m);
    return read;
}

// One property: whether it changed something.
typedef bool (*property_reader)(void *target, const char *name, sd_bus_message *m);

static bool read_adapter_property(void *target, const char *name, sd_bus_message *m)
{
    struct vela_bt_adapter *adapter = target;
    const char *text;
    int flag;
    if (strcmp(name, "Address") == 0 && read_variant(m, 's', &text)) {
        return set_text(&adapter->address, text);
    }
    if (strcmp(name, "Alias") == 0 && read_variant(m, 's', &text)) {
        return set_text(&adapter->name, text);
    }
    if (strcmp(name, "PowerState") == 0 && read_variant(m, 's', &text)) {
        return set_text(&adapter->power_state, text);
    }
    if (strcmp(name, "Powered") == 0 && read_variant(m, 'b', &flag)) {
        return set_bool(&adapter->powered, flag);
    }
    if (strcmp(name, "Discoverable") == 0 && read_variant(m, 'b', &flag)) {
        return set_bool(&adapter->discoverable, flag);
    }
    if (strcmp(name, "Discovering") == 0 && read_variant(m, 'b', &flag)) {
        return set_bool(&adapter->discovering, flag);
    }
    sd_bus_message_skip(m, "v");
    return false;
}

static bool read_device_property(void *target, const char *name, sd_bus_message *m)
{
    struct vela_bt_device *device = target;
    const char *text;
    int flag;
    uint32_t number;
    int16_t rssi;
    if (strcmp(name, "Address") == 0 && read_variant(m, 's', &text)) {
        return set_text(&device->address, text);
    }
    if (strcmp(name, "Alias") == 0 && read_variant(m, 's', &text)) {
        return set_text(&device->name, text);
    }
    if (strcmp(name, "Icon") == 0 && read_variant(m, 's', &text)) {
        return set_text(&device->icon, text);
    }
    if (strcmp(name, "Adapter") == 0 && read_variant(m, 'o', &text)) {
        return set_text(&device->adapter, text);
    }
    if (strcmp(name, "Class") == 0 && read_variant(m, 'u', &number)) {
        bool changed = device->class_of_device != number;
        device->class_of_device = number;
        return changed;
    }
    if (strcmp(name, "Paired") == 0 && read_variant(m, 'b', &flag)) {
        return set_bool(&device->paired, flag);
    }
    if (strcmp(name, "Trusted") == 0 && read_variant(m, 'b', &flag)) {
        return set_bool(&device->trusted, flag);
    }
    if (strcmp(name, "Connected") == 0 && read_variant(m, 'b', &flag)) {
        return set_bool(&device->connected, flag);
    }
    if (strcmp(name, "Blocked") == 0 && read_variant(m, 'b', &flag)) {
        return set_bool(&device->blocked, flag);
    }
    if (strcmp(name, "RSSI") == 0 && read_variant(m, 'n', &rssi)) {
        bool changed = !device->has_rssi || device->rssi != rssi;
        device->has_rssi = true;
        device->rssi = rssi;
        return changed;
    }
    sd_bus_message_skip(m, "v");
    return false;
}

static bool read_battery_property(void *target, const char *name, sd_bus_message *m)
{
    struct vela_bt_device *device = target;
    uint8_t percent;
    if (strcmp(name, "Percentage") == 0 && read_variant(m, 'y', &percent)) {
        int value = percent > 100 ? 100 : percent;
        bool changed = device->battery != value;
        device->battery = value;
        return changed;
    }
    sd_bus_message_skip(m, "v");
    return false;
}

// An a{sv} at the message's position. Negative errno, or whether something
// changed.
static int read_properties(sd_bus_message *m, property_reader reader, void *target)
{
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0) {
        return r;
    }
    bool changed = false;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *name;
        r = sd_bus_message_read_basic(m, 's', &name);
        if (r < 0) {
            return r;
        }
        changed |= reader(target, name, m);
        r = sd_bus_message_exit_container(m);
        if (r < 0) {
            return r;
        }
    }
    if (r < 0) {
        return r;
    }
    r = sd_bus_message_exit_container(m);
    return r < 0 ? r : changed;
}

static long hci_number(const char *path)
{
    const char *name = vela_bt_hci_name(path);
    return name ? strtol(name + 3, NULL, 10) : 1L << 20;
}

struct vela_bt_adapter *vela_bt_adapter_find(struct vela_bt_daemon *d, const char *path)
{
    for (size_t i = 0; path && i < d->adapter_count; ++i) {
        if (strcmp(d->adapters[i]->path, path) == 0) {
            return d->adapters[i];
        }
    }
    return NULL;
}

struct vela_bt_adapter *vela_bt_adapter_in_use(struct vela_bt_daemon *d)
{
    return vela_bt_adapter_find(d, d->in_use);
}

struct vela_bt_device *vela_bt_device_find(struct vela_bt_daemon *d, const char *path)
{
    for (size_t i = 0; path && i < d->device_count; ++i) {
        if (strcmp(d->devices[i]->path, path) == 0) {
            return d->devices[i];
        }
    }
    return NULL;
}

bool vela_bt_device_listed(struct vela_bt_daemon *d, const struct vela_bt_device *device)
{
    return d->in_use && device->adapter && strcmp(device->adapter, d->in_use) == 0;
}

bool vela_bt_available(const struct vela_bt_daemon *d)
{
    return d->bluez && d->adapter_count > 0;
}

void vela_bt_properties_changed(struct vela_bt_daemon *d)
{
    d->properties_changed = true;
    sd_event_source_set_enabled(d->flush, SD_EVENT_ONESHOT);
}

void vela_bt_touch(struct vela_bt_daemon *d, struct vela_bt_device *device)
{
    device->changed = true;
    sd_event_source_set_enabled(d->flush, SD_EVENT_ONESHOT);
}

static struct vela_bt_adapter *add_adapter(struct vela_bt_daemon *d, const char *path)
{
    struct vela_bt_adapter *adapter = calloc(1, sizeof(*adapter));
    struct vela_bt_adapter **adapters = realloc(d->adapters, (d->adapter_count + 1) * sizeof(*adapters));
    if (!adapter || !adapters || !(adapter->path = strdup(path))) {
        free(adapter);
        if (adapters) {
            d->adapters = adapters;
        }
        return NULL;
    }
    d->adapters = adapters;
    // In BlueZ's order: hci0 first.
    size_t at = d->adapter_count;
    while (at > 0 && hci_number(d->adapters[at - 1]->path) > hci_number(path)) {
        d->adapters[at] = d->adapters[at - 1];
        --at;
    }
    d->adapters[at] = adapter;
    ++d->adapter_count;
    return adapter;
}

static struct vela_bt_device *add_device(struct vela_bt_daemon *d, const char *path)
{
    struct vela_bt_device *device = calloc(1, sizeof(*device));
    struct vela_bt_device **devices = realloc(d->devices, (d->device_count + 1) * sizeof(*devices));
    if (!device || !devices || !(device->path = strdup(path))) {
        free(device);
        if (devices) {
            d->devices = devices;
        }
        return NULL;
    }
    device->battery = -1;
    d->devices = devices;
    d->devices[d->device_count++] = device;
    return device;
}

static void free_adapter(struct vela_bt_adapter *adapter)
{
    free(adapter->path);
    free(adapter->address);
    free(adapter->name);
    free(adapter->power_state);
    free(adapter);
}

static void free_device(struct vela_bt_device *device)
{
    free(device->path);
    free(device->adapter);
    free(device->address);
    free(device->name);
    free(device->icon);
    free(device);
}

static void note_removed(struct vela_bt_daemon *d, const char *path)
{
    char **removed = realloc(d->removed, (d->removed_count + 1) * sizeof(*removed));
    if (!removed) {
        return;
    }
    d->removed = removed;
    if ((d->removed[d->removed_count] = strdup(path))) {
        ++d->removed_count;
    }
    sd_event_source_set_enabled(d->flush, SD_EVENT_ONESHOT);
}

void vela_bt_remove_device(struct vela_bt_daemon *d, struct vela_bt_device *device)
{
    vela_bt_question_close(d, device->path, "device-gone");
    vela_bt_operations_device_gone(d, device);
    if (vela_bt_device_listed(d, device)) {
        note_removed(d, device->path);
    }
    for (size_t i = 0; i < d->device_count; ++i) {
        if (d->devices[i] == device) {
            d->devices[i] = d->devices[--d->device_count];
            break;
        }
    }
    free_device(device);
}

void vela_bt_remove_adapter(struct vela_bt_daemon *d, struct vela_bt_adapter *adapter)
{
    struct vela_bt_question *question = vela_bt_question_on(d, NULL);
    struct vela_bt_device *asking = question ? vela_bt_device_find(d, question->device) : NULL;
    if (asking && same(asking->adapter, adapter->path)) {
        vela_bt_question_close(d, NULL, "bluetooth-gone");
    }
    // BlueZ removes the devices first; any left go with their adapter.
    for (size_t i = d->device_count; i-- > 0;) {
        struct vela_bt_device *device = d->devices[i];
        if (same(device->adapter, adapter->path)) {
            if (device->operation) {
                vela_bt_operation_end(d, device, &vela_bt_adapter_gone, false);
            }
            vela_bt_remove_device(d, device);
        }
    }
    for (size_t i = 0; i < d->adapter_count; ++i) {
        if (d->adapters[i] == adapter) {
            memmove(&d->adapters[i], &d->adapters[i + 1], (d->adapter_count - i - 1) * sizeof(*d->adapters));
            --d->adapter_count;
            break;
        }
    }
    free_adapter(adapter);
    vela_bt_choose_adapter(d);
}

void vela_bt_clear(struct vela_bt_daemon *d)
{
    vela_bt_question_close(d, NULL, "bluetooth-gone");
    for (size_t i = d->device_count; i-- > 0;) {
        if (d->devices[i]->operation) {
            vela_bt_operation_end(d, d->devices[i], &vela_bt_not_running, false);
        }
    }
    vela_bt_power_end(d, &vela_bt_not_running, false);
    while (d->device_count > 0) {
        vela_bt_remove_device(d, d->devices[d->device_count - 1]);
    }
    while (d->adapter_count > 0) {
        struct vela_bt_adapter *adapter = d->adapters[--d->adapter_count];
        free_adapter(adapter);
    }
    vela_bt_choose_adapter(d);
}

void vela_bt_choose_adapter(struct vela_bt_daemon *d)
{
    struct vela_bt_adapter *choice = d->adapter_count > 0 ? d->adapters[0] : NULL;
    for (size_t i = 0; d->chosen && i < d->adapter_count; ++i) {
        if (same(d->adapters[i]->address, d->chosen)) {
            choice = d->adapters[i];
            break;
        }
    }
    const char *path = choice ? choice->path : NULL;
    if (same(path, d->in_use) && (path != NULL) == (d->in_use != NULL)) {
        return;
    }
    // The old adapter's devices leave the list, the new one's come in.
    for (size_t i = 0; i < d->device_count; ++i) {
        if (vela_bt_device_listed(d, d->devices[i])) {
            note_removed(d, d->devices[i]->path);
        }
    }
    free(d->in_use);
    d->in_use = path ? strdup(path) : NULL;
    if (path) {
        vela_bt_log("adapter in use: %s (%s)", path, choice->address ? choice->address : "?");
    }
    for (size_t i = 0; i < d->device_count; ++i) {
        if (vela_bt_device_listed(d, d->devices[i])) {
            vela_bt_touch(d, d->devices[i]);
        }
    }
    vela_bt_properties_changed(d);
}

// After an adapter's properties changed.
static void adapter_changed(struct vela_bt_daemon *d, struct vela_bt_adapter *adapter, bool was_powered)
{
    if (was_powered && !adapter->powered) {
        vela_bt_operations_adapter_off(d, adapter->path);
    }
    vela_bt_power_adapter_changed(d);
    if (same(adapter->path, d->in_use)) {
        vela_bt_properties_changed(d);
    }
}

int vela_bt_read_object(struct vela_bt_daemon *d, const char *path, sd_bus_message *m)
{
    int r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
    if (r < 0) {
        return r;
    }
    // Battery1 may come before Device1: it's applied once the device exists.
    bool has_battery = false;
    struct vela_bt_device battery = { .battery = -1 };
    bool adapters_changed = false;
    while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
        const char *interface;
        if ((r = sd_bus_message_read_basic(m, 's', &interface)) < 0) {
            return r;
        }
        if (strcmp(interface, ADAPTER1) == 0) {
            struct vela_bt_adapter *adapter = vela_bt_adapter_find(d, path);
            bool added = !adapter;
            if (added && !(adapter = add_adapter(d, path))) {
                return -ENOMEM;
            }
            bool was_powered = adapter->powered;
            if ((r = read_properties(m, read_adapter_property, adapter)) < 0) {
                return r;
            }
            adapter->seen = true;
            adapters_changed |= added || r > 0;
            if (added) {
                vela_bt_log("adapter %s (%s)", path, adapter->address ? adapter->address : "?");
                vela_bt_choose_adapter(d);
            }
            adapter_changed(d, adapter, was_powered);
        } else if (strcmp(interface, DEVICE1) == 0) {
            struct vela_bt_device *device = vela_bt_device_find(d, path);
            if (!device && !(device = add_device(d, path))) {
                return -ENOMEM;
            }
            if ((r = read_properties(m, read_device_property, device)) < 0) {
                return r;
            }
            device->seen = true;
            if (vela_bt_device_listed(d, device)) {
                vela_bt_touch(d, device);
            }
            if (r > 0) {
                vela_bt_operations_device_changed(d, device);
            }
        } else if (strcmp(interface, BATTERY1) == 0) {
            if ((r = read_properties(m, read_battery_property, &battery)) < 0) {
                return r;
            }
            has_battery = true;
        } else if ((r = sd_bus_message_skip(m, "a{sv}")) < 0) {
            return r;
        }
        if ((r = sd_bus_message_exit_container(m)) < 0) {
            return r;
        }
    }
    if (r < 0) {
        return r;
    }
    struct vela_bt_device *device = vela_bt_device_find(d, path);
    if (has_battery && device && device->battery != battery.battery) {
        device->battery = battery.battery;
        vela_bt_touch(d, device);
    }
    if (adapters_changed) {
        vela_bt_properties_changed(d);
    }
    return sd_bus_message_exit_container(m);
}

int vela_bt_read_changes(struct vela_bt_daemon *d, const char *path, sd_bus_message *m)
{
    const char *interface;
    int r = sd_bus_message_read_basic(m, 's', &interface);
    if (r < 0) {
        return r;
    }
    struct vela_bt_adapter *adapter = vela_bt_adapter_find(d, path);
    struct vela_bt_device *device = vela_bt_device_find(d, path);
    property_reader reader = NULL;
    void *target = NULL;
    if (strcmp(interface, ADAPTER1) == 0 && adapter) {
        reader = read_adapter_property;
        target = adapter;
    } else if (strcmp(interface, DEVICE1) == 0 && device) {
        reader = read_device_property;
        target = device;
    } else if (strcmp(interface, BATTERY1) == 0 && device) {
        reader = read_battery_property;
        target = device;
    } else {
        return 0;
    }
    bool was_powered = adapter && adapter->powered;
    r = read_properties(m, reader, target);
    if (r < 0) {
        return r;
    }
    bool changed = r > 0;
    // Invalidated: RSSI once the search ends, the battery level.
    if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0) {
        return r;
    }
    const char *name;
    while ((r = sd_bus_message_read_basic(m, 's', &name)) > 0) {
        if (device && reader == read_device_property && strcmp(name, "RSSI") == 0 && device->has_rssi) {
            device->has_rssi = false;
            changed = true;
        } else if (device && reader == read_battery_property && strcmp(name, "Percentage") == 0
            && device->battery >= 0) {
            device->battery = -1;
            changed = true;
        }
    }
    if (r < 0) {
        return r;
    }
    sd_bus_message_exit_container(m);
    if (reader == read_adapter_property) {
        if (changed) {
            adapter_changed(d, adapter, was_powered);
        }
    } else if (changed) {
        if (vela_bt_device_listed(d, device)) {
            vela_bt_touch(d, device);
        }
        vela_bt_operations_device_changed(d, device);
    }
    return 0;
}

int vela_bt_read_removal(struct vela_bt_daemon *d, const char *path, sd_bus_message *m)
{
    int r = sd_bus_message_enter_container(m, 'a', "s");
    if (r < 0) {
        return r;
    }
    const char *interface;
    while ((r = sd_bus_message_read_basic(m, 's', &interface)) > 0) {
        struct vela_bt_device *device = vela_bt_device_find(d, path);
        struct vela_bt_adapter *adapter = vela_bt_adapter_find(d, path);
        if (strcmp(interface, DEVICE1) == 0 && device) {
            vela_bt_remove_device(d, device);
        } else if (strcmp(interface, BATTERY1) == 0 && device && device->battery >= 0) {
            device->battery = -1;
            if (vela_bt_device_listed(d, device)) {
                vela_bt_touch(d, device);
            }
        } else if (strcmp(interface, ADAPTER1) == 0 && adapter) {
            vela_bt_log("adapter %s removed", path);
            vela_bt_remove_adapter(d, adapter);
            vela_bt_power_adapter_changed(d);
            vela_bt_platform_refresh(d);
            vela_bt_properties_changed(d);
        }
    }
    if (r < 0) {
        return r;
    }
    return sd_bus_message_exit_container(m);
}

void vela_bt_append_entry(sd_bus_message *m, const char *name, const char *type, ...)
{
    va_list values;
    va_start(values, type);
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append_basic(m, 's', name);
    sd_bus_message_open_container(m, 'v', type);
    sd_bus_message_appendv(m, type, values);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    va_end(values);
}

void vela_bt_append_device(sd_bus_message *m, const struct vela_bt_device *device)
{
    sd_bus_message_open_container(m, 'a', "{sv}");
    vela_bt_append_entry(m, "address", "s", device->address ? device->address : "");
    vela_bt_append_entry(m, "name", "s", device->name ? device->name : "");
    vela_bt_append_entry(m, "icon", "s", device->icon && *device->icon ? device->icon : "bluetooth");
    vela_bt_append_entry(m, "type", "s", vela_bt_device_type(device->icon, device->class_of_device));
    vela_bt_append_entry(m, "paired", "b", (int)device->paired);
    vela_bt_append_entry(m, "trusted", "b", (int)device->trusted);
    vela_bt_append_entry(m, "connected", "b", (int)device->connected);
    vela_bt_append_entry(m, "blocked", "b", (int)device->blocked);
    if (device->battery >= 0) {
        vela_bt_append_entry(m, "battery", "y", (uint8_t)device->battery);
    }
    if (device->has_rssi) {
        vela_bt_append_entry(m, "rssi", "n", device->rssi);
    }
    if (device->operation) {
        vela_bt_append_entry(m, "operation", "s", vela_bt_step_state(device->operation->step));
    }
    if (device->failure) {
        vela_bt_append_entry(m, "error", "s", device->failure->code);
        vela_bt_append_entry(m, "message", "s", device->failure->message);
    }
    sd_bus_message_close_container(m);
}

void vela_bt_append_adapter(sd_bus_message *m, const struct vela_bt_adapter *adapter)
{
    sd_bus_message_open_container(m, 'a', "{sv}");
    if (adapter) {
        vela_bt_append_entry(m, "path", "o", adapter->path);
        vela_bt_append_entry(m, "address", "s", adapter->address ? adapter->address : "");
        vela_bt_append_entry(m, "name", "s", adapter->name ? adapter->name : "");
        vela_bt_append_entry(m, "powered", "b", (int)adapter->powered);
        vela_bt_append_entry(m, "powerState", "s", adapter->power_state ? adapter->power_state : "");
        vela_bt_append_entry(m, "discoverable", "b", (int)adapter->discoverable);
        vela_bt_append_entry(m, "discovering", "b", (int)adapter->discovering);
    }
    sd_bus_message_close_container(m);
}

void vela_bt_model_free(struct vela_bt_daemon *d)
{
    for (size_t i = 0; i < d->device_count; ++i) {
        free_device(d->devices[i]);
    }
    for (size_t i = 0; i < d->adapter_count; ++i) {
        free_adapter(d->adapters[i]);
    }
    for (size_t i = 0; i < d->removed_count; ++i) {
        free(d->removed[i]);
    }
    free(d->devices);
    free(d->adapters);
    free(d->removed);
    free(d->in_use);
    free(d->chosen);
    free(d->bluez);
    d->devices = NULL;
    d->adapters = NULL;
    d->removed = NULL;
    d->device_count = d->adapter_count = d->removed_count = 0;
}
