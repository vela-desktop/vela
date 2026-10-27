// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Following BlueZ on the system bus (docs/bluetooth.md §6.2): its name, then
// GetManagedObjects and the ObjectManager and Properties signals. Every read
// is asynchronous.

#include "daemon.h"

#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define BLUEZ "org.bluez"

int vela_bt_call(struct vela_bt_daemon *d, sd_bus_slot **slot, const char *path, const char *interface,
    const char *member, uint64_t timeout, sd_bus_message_handler_t reply, void *data, const char *types, ...)
{
    if (!d->bluez) {
        return -ENOTCONN;
    }
    sd_bus_message *call = NULL;
    int r = sd_bus_message_new_method_call(d->system, &call, BLUEZ, path, interface, member);
    if (r >= 0 && types) {
        va_list values;
        va_start(values, types);
        r = sd_bus_message_appendv(call, types, values);
        va_end(values);
    }
    if (r >= 0) {
        r = sd_bus_call_async(d->system, slot, call, reply, data, timeout);
    }
    sd_bus_message_unref(call);
    if (r < 0) {
        vela_bt_log("%s.%s on %s: %s", interface, member, path, strerror(-r));
    }
    return r;
}

// Only signals from BlueZ's current owner count.
static bool from_bluez(struct vela_bt_daemon *d, sd_bus_message *m)
{
    const char *sender = sd_bus_message_get_sender(m);
    return d->bluez && sender && strcmp(sender, d->bluez) == 0;
}

static int on_managed_objects(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    d->managed = sd_bus_slot_unref(d->managed);
    if (sd_bus_message_is_method_error(reply, NULL)) {
        vela_bt_log("GetManagedObjects: %s", sd_bus_message_get_error(reply)->message);
        return 0;
    }
    // Rebuilt: what BlueZ no longer has goes.
    for (size_t i = 0; i < d->adapter_count; ++i) {
        d->adapters[i]->seen = false;
    }
    for (size_t i = 0; i < d->device_count; ++i) {
        d->devices[i]->seen = false;
    }
    int r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    while (r >= 0 && (r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
        const char *path;
        r = sd_bus_message_read_basic(reply, 'o', &path);
        if (r >= 0) {
            r = vela_bt_read_object(d, path, reply);
        }
        if (r >= 0) {
            r = sd_bus_message_exit_container(reply);
        }
    }
    if (r < 0) {
        vela_bt_log("GetManagedObjects: bad reply: %s", strerror(-r));
    }
    for (size_t i = d->device_count; i-- > 0;) {
        if (!d->devices[i]->seen) {
            vela_bt_remove_device(d, d->devices[i]);
        }
    }
    for (size_t i = d->adapter_count; i-- > 0;) {
        if (!d->adapters[i]->seen) {
            vela_bt_remove_adapter(d, d->adapters[i]);
        }
    }
    vela_bt_choose_adapter(d);
    vela_bt_power_adapter_changed(d);
    vela_bt_platform_refresh(d);
    vela_bt_properties_changed(d);
    vela_bt_log("BlueZ: %zu adapters, %zu devices", d->adapter_count, d->device_count);
    vela_bt_agent_update(d);
    return 0;
}

static void appeared(struct vela_bt_daemon *d, const char *owner)
{
    free(d->bluez);
    d->bluez = strdup(owner);
    vela_bt_log("BlueZ is on the bus (%s)", owner);
    d->managed = sd_bus_slot_unref(d->managed);
    vela_bt_call(d, &d->managed, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
        VELA_BT_STEP_TIMEOUT_US, on_managed_objects, d, NULL);
}

static void vanished(struct vela_bt_daemon *d)
{
    vela_bt_log("BlueZ left the bus");
    d->managed = sd_bus_slot_unref(d->managed);
    vela_bt_clear(d);
    free(d->bluez);
    d->bluez = NULL;
    vela_bt_agent_update(d);
    vela_bt_platform_refresh(d);
    vela_bt_properties_changed(d);
}

static int on_owner_changed(sd_bus_message *m, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *name, *old_owner, *new_owner;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0 || strcmp(name, BLUEZ) != 0) {
        return 0;
    }
    if (d->bluez && (!*new_owner || strcmp(new_owner, d->bluez) != 0)) {
        vanished(d);
    }
    if (*new_owner && !d->bluez) {
        appeared(d, new_owner);
    }
    return 0;
}

static int on_name_owner(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *owner;
    if (!sd_bus_message_is_method_error(reply, NULL) && sd_bus_message_read(reply, "s", &owner) > 0 && !d->bluez) {
        appeared(d, owner);
    } else if (!d->bluez) {
        vela_bt_log("BlueZ isn't running: waiting for it");
    }
    return 0;
}

static int on_interfaces_added(sd_bus_message *m, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *path;
    if (!from_bluez(d, m) || sd_bus_message_read_basic(m, 'o', &path) < 0) {
        return 0;
    }
    if (vela_bt_read_object(d, path, m) < 0) {
        vela_bt_log("InterfacesAdded on %s: bad message", path);
    }
    vela_bt_platform_refresh(d);
    return 0;
}

static int on_interfaces_removed(sd_bus_message *m, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *path;
    if (from_bluez(d, m) && sd_bus_message_read_basic(m, 'o', &path) >= 0) {
        vela_bt_read_removal(d, path, m);
    }
    return 0;
}

static int on_properties_changed(sd_bus_message *m, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *path = sd_bus_message_get_path(m);
    if (from_bluez(d, m) && path) {
        vela_bt_read_changes(d, path, m);
    }
    return 0;
}

int vela_bt_bluez_start(struct vela_bt_daemon *d)
{
    // The matches go to the bus before GetNameOwner: nothing is missed
    // between the two.
    int r = sd_bus_add_match_async(d->system, NULL,
        "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
        "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" BLUEZ "'",
        on_owner_changed, NULL, d);
    if (r >= 0) {
        r = sd_bus_match_signal_async(d->system, NULL, BLUEZ, NULL, "org.freedesktop.DBus.ObjectManager",
            "InterfacesAdded", on_interfaces_added, NULL, d);
    }
    if (r >= 0) {
        r = sd_bus_match_signal_async(d->system, NULL, BLUEZ, NULL, "org.freedesktop.DBus.ObjectManager",
            "InterfacesRemoved", on_interfaces_removed, NULL, d);
    }
    if (r >= 0) {
        r = sd_bus_match_signal_async(d->system, NULL, BLUEZ, NULL, "org.freedesktop.DBus.Properties",
            "PropertiesChanged", on_properties_changed, NULL, d);
    }
    if (r >= 0) {
        r = sd_bus_call_method_async(d->system, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "GetNameOwner", on_name_owner, d, "s", BLUEZ);
    }
    return r;
}
