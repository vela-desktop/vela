// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// org.vela.Bluetooth on the session bus (docs/bluetooth.md §7). Methods
// that change something return once it's done: the D-Bus reply carries
// the result, OperationChanged the steps on the way. While the session is
// not in front (§6.7) they return Error.Inactive.

#include "daemon.h"

#include "caller.h"
#include "config.h"
#include "describe.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define INTERFACE VELA_BT_NAME

static int get_available(sd_bus *bus, const char *path, const char *interface, const char *property,
    sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    return sd_bus_message_append(reply, "b", (int)vela_bt_available(d));
}

static int get_platform_blocked(sd_bus *bus, const char *path, const char *interface, const char *property,
    sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    return sd_bus_message_append(reply, "b", (int)d->platform_blocked);
}

static int get_active(sd_bus *bus, const char *path, const char *interface, const char *property,
    sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    return sd_bus_message_append(reply, "b", (int)vela_bt_active(d));
}

static int get_adapters(sd_bus *bus, const char *path, const char *interface, const char *property,
    sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    int r = sd_bus_message_open_container(reply, 'a', "a{sv}");
    for (size_t i = 0; r >= 0 && i < d->adapter_count; ++i) {
        const struct vela_bt_adapter *adapter = d->adapters[i];
        r = sd_bus_message_append(reply, "a{sv}", 4, "path", "o", adapter->path, "address", "s",
            adapter->address ? adapter->address : "", "name", "s", adapter->name ? adapter->name : "", "inUse", "b",
            (int)(d->in_use && strcmp(d->in_use, adapter->path) == 0));
    }
    return r < 0 ? r : sd_bus_message_close_container(reply);
}

static int get_adapter(sd_bus *bus, const char *path, const char *interface, const char *property,
    sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    vela_bt_append_adapter(reply, vela_bt_adapter_in_use(d));
    return 0;
}

static void append_devices(struct vela_bt_daemon *d, sd_bus_message *m, bool only_changed)
{
    sd_bus_message_open_container(m, 'a', "{oa{sv}}");
    for (size_t i = 0; i < d->device_count; ++i) {
        struct vela_bt_device *device = d->devices[i];
        if (vela_bt_device_listed(d, device) && (!only_changed || device->changed)) {
            sd_bus_message_open_container(m, 'e', "oa{sv}");
            sd_bus_message_append_basic(m, 'o', device->path);
            vela_bt_append_device(m, device);
            sd_bus_message_close_container(m);
        }
    }
    sd_bus_message_close_container(m);
}

// The caller passes `trust` (§7.3), and the session is in front.
static int check_acting(struct vela_bt_daemon *d, sd_bus_message *call, enum vela_bt_trust trust, sd_bus_error *error)
{
    int r = vela_bt_check_caller(call, trust, d->ui, error);
    if (r >= 0 && !vela_bt_active(d)) {
        r = sd_bus_error_set(error, VELA_BT_ERROR("Inactive"), "Bluetooth is in use by another session.");
    }
    return r;
}

// A device of the adapter in use, or an error.
static struct vela_bt_device *listed_device(struct vela_bt_daemon *d, const char *path, sd_bus_error *error)
{
    struct vela_bt_device *device = vela_bt_device_find(d, path);
    if (!device || !vela_bt_device_listed(d, device)) {
        sd_bus_error_set(error, VELA_BT_ERROR("UnknownDevice"), path);
        return NULL;
    }
    return device;
}

static int method_devices(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    int r = vela_bt_check_caller(call, VELA_BT_SAME_USER, d->ui, error);
    if (r < 0) {
        return r;
    }
    sd_bus_message *reply = NULL;
    if ((r = sd_bus_message_new_method_return(call, &reply)) < 0) {
        return r;
    }
    append_devices(d, reply, false);
    r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return r;
}

static int method_set_powered(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    int on;
    int r = sd_bus_message_read(call, "b", &on);
    if (r >= 0) {
        r = check_acting(d, call, VELA_BT_UNSANDBOXED, error);
    }
    if (r < 0) {
        return r;
    }
    return vela_bt_power_set(d, call, on, error);
}

static int method_use_adapter(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *address;
    int r = sd_bus_message_read(call, "s", &address);
    if (r >= 0) {
        r = check_acting(d, call, VELA_BT_UNSANDBOXED, error);
    }
    if (r < 0) {
        return r;
    }
    bool found = false;
    for (size_t i = 0; i < d->adapter_count; ++i) {
        found |= d->adapters[i]->address && strcmp(d->adapters[i]->address, address) == 0;
    }
    if (!found) {
        return sd_bus_error_set(error, VELA_BT_ERROR("UnknownAdapter"), address);
    }
    if (d->pairing || d->power.active || d->question) {
        const char *busy = d->pairing ? d->pairing->path : d->question ? d->question->device : "";
        return sd_bus_error_set(error, VELA_BT_ERROR("Busy"), busy);
    }
    char *chosen = strdup(address);
    if (!chosen) {
        return -ENOMEM;
    }
    free(d->chosen);
    d->chosen = chosen;
    // Remembered by address: a USB dongle that comes back is picked again.
    vela_config_write("bluetooth-adapter", address);
    vela_bt_choose_adapter(d);
    vela_bt_flush(d);
    return sd_bus_reply_method_return(call, NULL);
}

static int start_operation(sd_bus_message *call, struct vela_bt_daemon *d, enum vela_bt_kind kind,
    enum vela_bt_trust trust, const char *path, const char *name, sd_bus_error *error)
{
    int r = check_acting(d, call, trust, error);
    if (r < 0) {
        return r;
    }
    struct vela_bt_device *device = listed_device(d, path, error);
    if (!device) {
        return -ENOENT;
    }
    return vela_bt_operation_start(d, call, device, kind, name, trust == VELA_BT_VELA_UI, error);
}

static int method_add(sd_bus_message *call, void *data, sd_bus_error *error)
{
    const char *path;
    int prompts_here;
    int r = sd_bus_message_read(call, "ob", &path, &prompts_here);
    if (r < 0) {
        return r;
    }
    // Drawing the questions is Vela's UI's (§7.3).
    return start_operation(
        call, data, VELA_BT_ADD, prompts_here ? VELA_BT_VELA_UI : VELA_BT_UNSANDBOXED, path, NULL, error);
}

static int method_device(sd_bus_message *call, void *data, sd_bus_error *error, enum vela_bt_kind kind)
{
    const char *path;
    int r = sd_bus_message_read(call, "o", &path);
    return r < 0 ? r : start_operation(call, data, kind, VELA_BT_UNSANDBOXED, path, NULL, error);
}

static int method_connect(sd_bus_message *call, void *data, sd_bus_error *error)
{
    return method_device(call, data, error, VELA_BT_CONNECT);
}

static int method_disconnect(sd_bus_message *call, void *data, sd_bus_error *error)
{
    return method_device(call, data, error, VELA_BT_DISCONNECT);
}

static int method_remove(sd_bus_message *call, void *data, sd_bus_error *error)
{
    return method_device(call, data, error, VELA_BT_REMOVE);
}

static int method_rename(sd_bus_message *call, void *data, sd_bus_error *error)
{
    const char *path, *name;
    int r = sd_bus_message_read(call, "os", &path, &name);
    if (r < 0) {
        return r;
    }
    if (!vela_bt_valid_name(name)) {
        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Not a valid name.");
    }
    return start_operation(call, data, VELA_BT_RENAME, VELA_BT_UNSANDBOXED, path, name, error);
}

static int method_cancel(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *path;
    int r = sd_bus_message_read(call, "o", &path);
    if (r >= 0) {
        r = vela_bt_check_caller(call, VELA_BT_UNSANDBOXED, d->ui, error);
    }
    if (r < 0) {
        return r;
    }
    struct vela_bt_device *device = listed_device(d, path, error);
    if (!device) {
        return -ENOENT;
    }
    vela_bt_operation_cancel(d, device, NULL);
    return sd_bus_reply_method_return(call, NULL);
}

static int method_questions(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    int r = vela_bt_check_caller(call, VELA_BT_VELA_UI, d->ui, error);
    return r < 0 ? r : vela_bt_questions(d, call);
}

static int method_answer(sd_bus_message *call, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    uint64_t id;
    const char *response, *value;
    int r = sd_bus_message_read(call, "tss", &id, &response, &value);
    if (r >= 0) {
        r = check_acting(d, call, VELA_BT_VELA_UI, error);
    }
    if (r < 0) {
        return r;
    }
    return vela_bt_answer(d, call, id, response, value, error);
}

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Available", "b", get_available, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("PlatformBlocked", "b", get_platform_blocked, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Active", "b", get_active, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Adapters", "aa{sv}", get_adapters, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Adapter", "a{sv}", get_adapter, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("Devices", "", "a{oa{sv}}", method_devices, 0),
    SD_BUS_METHOD("SetPowered", "b", "", method_set_powered, 0),
    SD_BUS_METHOD("UseAdapter", "s", "", method_use_adapter, 0),
    SD_BUS_METHOD("Add", "ob", "", method_add, 0),
    SD_BUS_METHOD("Connect", "o", "", method_connect, 0),
    SD_BUS_METHOD("Disconnect", "o", "", method_disconnect, 0),
    SD_BUS_METHOD("Remove", "o", "", method_remove, 0),
    SD_BUS_METHOD("Rename", "os", "", method_rename, 0),
    SD_BUS_METHOD("Cancel", "o", "", method_cancel, 0),
    SD_BUS_METHOD("Questions", "", "a(toa{sv})", method_questions, 0),
    SD_BUS_METHOD("Answer", "tss", "", method_answer, 0),
    SD_BUS_SIGNAL("DevicesChanged", "a{oa{sv}}ao", 0),
    SD_BUS_SIGNAL("OperationChanged", "osss", 0),
    // Sent to each of Vela's UI clients only (§7.3).
    SD_BUS_SIGNAL("Question", "toa{sv}", 0),
    SD_BUS_SIGNAL("QuestionChanged", "ta{sv}", 0),
    SD_BUS_SIGNAL("QuestionClosed", "ts", 0),
    SD_BUS_VTABLE_END,
};

void vela_bt_flush(struct vela_bt_daemon *d)
{
    sd_event_source_set_enabled(d->flush, SD_EVENT_OFF);
    bool devices = d->removed_count > 0;
    for (size_t i = 0; !devices && i < d->device_count; ++i) {
        devices = d->devices[i]->changed && vela_bt_device_listed(d, d->devices[i]);
    }
    if (devices) {
        sd_bus_message *m = NULL;
        if (sd_bus_message_new_signal(d->session, &m, VELA_BT_PATH, INTERFACE, "DevicesChanged") >= 0) {
            append_devices(d, m, true);
            sd_bus_message_open_container(m, 'a', "o");
            for (size_t i = 0; i < d->removed_count; ++i) {
                // Gone and back before this signal: it's in the changed list.
                struct vela_bt_device *device = vela_bt_device_find(d, d->removed[i]);
                if (!device || !vela_bt_device_listed(d, device)) {
                    sd_bus_message_append_basic(m, 'o', d->removed[i]);
                }
            }
            sd_bus_message_close_container(m);
            sd_bus_send(d->session, m, NULL);
            sd_bus_message_unref(m);
        }
    }
    for (size_t i = 0; i < d->device_count; ++i) {
        d->devices[i]->changed = false;
    }
    for (size_t i = 0; i < d->removed_count; ++i) {
        free(d->removed[i]);
    }
    d->removed_count = 0;
    if (d->properties_changed) {
        d->properties_changed = false;
        sd_bus_emit_properties_changed(
            d->session, VELA_BT_PATH, INTERFACE, "Available", "PlatformBlocked", "Active", "Adapters", "Adapter", NULL);
    }
}

static int on_flush(sd_event_source *source, void *data)
{
    vela_bt_flush(data);
    return 0;
}

void vela_bt_emit_operation(struct vela_bt_daemon *d, const struct vela_bt_device *device, const char *state,
    const struct vela_bt_failure *failure)
{
    // The device's state goes out first.
    vela_bt_flush(d);
    sd_bus_emit_signal(d->session, VELA_BT_PATH, INTERFACE, "OperationChanged", "osss", device->path, state,
        failure ? failure->code : "", failure ? failure->message : "");
}

int vela_bt_service_start(struct vela_bt_daemon *d)
{
    int r = sd_event_add_defer(d->event, &d->flush, on_flush, d);
    if (r >= 0) {
        r = sd_event_source_set_enabled(d->flush, SD_EVENT_OFF);
    }
    if (r >= 0) {
        r = sd_bus_add_object_vtable(d->session, NULL, VELA_BT_PATH, INTERFACE, vtable, d);
    }
    if (r >= 0) {
        r = sd_bus_request_name(d->session, VELA_BT_NAME, 0);
    }
    return r;
}
