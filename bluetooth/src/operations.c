// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Operations on devices (docs/bluetooth.md §6.3): one per device, one Add
// at a time (§6.5). Each ends in done, failed or cancelled, also when the
// device, the adapter or BlueZ goes away.

#include "daemon.h"

#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEVICE1 "org.bluez.Device1"

uint64_t vela_bt_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

void vela_bt_waiters_add(struct vela_bt_waiters *waiters, sd_bus_message *call)
{
    if (!call) {
        return;
    }
    sd_bus_message **calls = realloc(waiters->calls, (waiters->count + 1) * sizeof(*calls));
    if (!calls) {
        sd_bus_reply_method_errno(call, ENOMEM, NULL);
        return;
    }
    waiters->calls = calls;
    waiters->calls[waiters->count++] = sd_bus_message_ref(call);
}

void vela_bt_waiters_reply(struct vela_bt_waiters *waiters, const struct vela_bt_failure *failure, bool cancelled)
{
    for (size_t i = 0; i < waiters->count; ++i) {
        sd_bus_message *call = waiters->calls[i];
        if (cancelled) {
            sd_bus_reply_method_error(call, &SD_BUS_ERROR_MAKE_CONST(VELA_BT_ERROR("Cancelled"), "Cancelled."));
        } else if (failure) {
            sd_bus_reply_method_error(call, &SD_BUS_ERROR_MAKE_CONST(VELA_BT_ERROR("Failed"), failure->message));
        } else {
            sd_bus_reply_method_return(call, NULL);
        }
        sd_bus_message_unref(call);
    }
    free(waiters->calls);
    waiters->calls = NULL;
    waiters->count = 0;
}

static void free_operation(struct vela_bt_operation *operation)
{
    sd_bus_slot_unref(operation->call);
    sd_event_source_disable_unref(operation->timer);
    free(operation->name);
    free(operation);
}

void vela_bt_operation_end(
    struct vela_bt_daemon *d, struct vela_bt_device *device, const struct vela_bt_failure *failure, bool cancelled)
{
    struct vela_bt_operation *operation = device->operation;
    if (!operation) {
        return;
    }
    device->operation = NULL;
    if (d->pairing == device) {
        d->pairing = NULL;
    }
    operation->call = sd_bus_slot_unref(operation->call);
    // A failed Trust stays shown after a successful Add.
    if (cancelled) {
        device->failure = NULL;
    } else if (failure) {
        device->failure = failure;
    }
    vela_bt_touch(d, device);
    const char *state = cancelled ? "cancelled" : failure ? "failed" : "done";
    vela_bt_log("%s: %s%s%s", device->path, state, failure && !cancelled ? ": " : "",
        failure && !cancelled ? failure->code : "");
    vela_bt_emit_operation(d, device, state, cancelled ? NULL : device->failure);
    vela_bt_waiters_reply(&operation->waiters, failure, cancelled);
    free_operation(operation);
}

static int on_reply(sd_bus_message *reply, void *data, sd_bus_error *error);

static int on_remove_timeout(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_bt_operation *operation = data;
    vela_bt_operation_end(operation->daemon, operation->device, &vela_bt_failed, false);
    return 0;
}

static void run_step(struct vela_bt_daemon *d, struct vela_bt_device *device, enum vela_bt_step step)
{
    struct vela_bt_operation *operation = device->operation;
    operation->step = step;
    vela_bt_touch(d, device);
    vela_bt_emit_operation(d, device, vela_bt_step_state(step), device->failure);
    sd_bus_slot **slot = &operation->call;
    int r;
    switch (step) {
    case VELA_BT_STEP_PAIR:
        r = vela_bt_call(d, slot, device->path, DEVICE1, "Pair", VELA_BT_PAIR_TIMEOUT_US, on_reply, operation, NULL);
        break;
    case VELA_BT_STEP_TRUST:
        r = vela_bt_call(d, slot, device->path, "org.freedesktop.DBus.Properties", "Set", VELA_BT_STEP_TIMEOUT_US,
            on_reply, operation, "ssv", DEVICE1, "Trusted", "b", 1);
        break;
    case VELA_BT_STEP_CONNECT:
        r = vela_bt_call(d, slot, device->path, DEVICE1, "Connect", VELA_BT_STEP_TIMEOUT_US, on_reply, operation, NULL);
        break;
    case VELA_BT_STEP_DISCONNECT:
        r = vela_bt_call(
            d, slot, device->path, DEVICE1, "Disconnect", VELA_BT_STEP_TIMEOUT_US, on_reply, operation, NULL);
        break;
    case VELA_BT_STEP_REMOVE:
        r = vela_bt_call(d, slot, device->adapter, "org.bluez.Adapter1", "RemoveDevice", VELA_BT_STEP_TIMEOUT_US,
            on_reply, operation, "o", device->path);
        break;
    case VELA_BT_STEP_RENAME:
        r = vela_bt_call(d, slot, device->path, "org.freedesktop.DBus.Properties", "Set", VELA_BT_STEP_TIMEOUT_US,
            on_reply, operation, "ssv", DEVICE1, "Alias", "s", operation->name);
        break;
    default:
        r = -EINVAL;
    }
    if (r < 0) {
        vela_bt_operation_end(d, device, r == -ENOTCONN ? &vela_bt_not_running : &vela_bt_failed, false);
    }
}

// An Add after `next` (§6.3): Pair -> Trust -> Connect.
static void add_continue(struct vela_bt_daemon *d, struct vela_bt_device *device, enum vela_bt_add_next next,
    const struct vela_bt_failure *failure)
{
    switch (next) {
    case VELA_BT_ADD_TRUST:
        if (!device->trusted) {
            run_step(d, device, VELA_BT_STEP_TRUST);
            return;
        }
        // fall through
    case VELA_BT_ADD_CONNECT:
        if (!device->connected) {
            run_step(d, device, VELA_BT_STEP_CONNECT);
            return;
        }
        // fall through
    case VELA_BT_ADD_DONE:
        vela_bt_operation_end(d, device, NULL, false);
        return;
    case VELA_BT_ADD_FAILED:
        vela_bt_operation_end(d, device, failure, false);
        return;
    }
}

static int on_reply(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_operation *operation = data;
    struct vela_bt_daemon *d = operation->daemon;
    struct vela_bt_device *device = operation->device;
    operation->call = sd_bus_slot_unref(operation->call);
    const struct vela_bt_failure *failure = NULL;
    if (sd_bus_message_is_method_error(reply, NULL)) {
        const sd_bus_error *cause = sd_bus_message_get_error(reply);
        failure = vela_bt_failure_of(operation->step, cause->name, cause->message);
        vela_bt_log("%s: %s: %s: %s", device->path, vela_bt_step_state(operation->step), cause->name,
            cause->message ? cause->message : "");
    }
    bool ok = !failure;

    switch (operation->kind) {
    case VELA_BT_ADD: {
        enum vela_bt_add_next next = vela_bt_add_next(operation->step, ok);
        if (operation->step == VELA_BT_STEP_TRUST && !ok) {
            device->failure = &vela_bt_not_trusted;
        }
        if (operation->step == VELA_BT_STEP_CONNECT && !ok) {
            add_continue(d, device, next, &vela_bt_paired_not_connected);
        } else {
            add_continue(d, device, next, failure);
        }
        break;
    }
    case VELA_BT_REMOVE:
        if (!ok) {
            vela_bt_operation_end(d, device, failure, false);
        } else {
            // Done once Device1 disappears (vela_bt_operations_device_gone).
            sd_event_add_time_relative(d->event, &operation->timer, CLOCK_MONOTONIC, VELA_BT_STEP_TIMEOUT_US, 0,
                on_remove_timeout, operation);
        }
        break;
    default:
        vela_bt_operation_end(d, device, failure, false);
    }
    return 0;
}

int vela_bt_operation_start(struct vela_bt_daemon *d, sd_bus_message *call, struct vela_bt_device *device,
    enum vela_bt_kind kind, const char *name, sd_bus_error *error)
{
    struct vela_bt_operation *running = device->operation;
    if (running) {
        // The same operation again joins it; anything else waits (§6.5).
        if (running->kind == kind && (kind != VELA_BT_RENAME || strcmp(running->name, name) == 0)) {
            vela_bt_waiters_add(&running->waiters, call);
            return 1;
        }
        return sd_bus_error_set(error, VELA_BT_ERROR("Busy"), device->path);
    }
    if (kind == VELA_BT_ADD && d->pairing && d->pairing != device) {
        return sd_bus_error_set(error, VELA_BT_ERROR("Busy"), d->pairing->path);
    }
    struct vela_bt_operation *operation = calloc(1, sizeof(*operation));
    if (!operation || (name && !(operation->name = strdup(name)))) {
        free(operation);
        return -ENOMEM;
    }
    operation->daemon = d;
    operation->device = device;
    operation->kind = kind;
    vela_bt_waiters_add(&operation->waiters, call);
    device->operation = operation;
    device->failure = NULL;
    if (kind == VELA_BT_ADD) {
        d->pairing = device;
    }
    struct vela_bt_adapter *adapter = vela_bt_adapter_find(d, device->adapter);
    if ((kind == VELA_BT_ADD || kind == VELA_BT_CONNECT) && (!adapter || !adapter->powered)) {
        operation->step = kind == VELA_BT_ADD ? VELA_BT_STEP_PAIR : VELA_BT_STEP_CONNECT;
        vela_bt_operation_end(d, device, &vela_bt_adapter_off, false);
        return 1;
    }
    static const enum vela_bt_step first[] = {
        [VELA_BT_ADD] = VELA_BT_STEP_PAIR,
        [VELA_BT_CONNECT] = VELA_BT_STEP_CONNECT,
        [VELA_BT_DISCONNECT] = VELA_BT_STEP_DISCONNECT,
        [VELA_BT_REMOVE] = VELA_BT_STEP_REMOVE,
        [VELA_BT_RENAME] = VELA_BT_STEP_RENAME,
    };
    run_step(d, device, first[kind]);
    return 1;
}

void vela_bt_operation_cancel(struct vela_bt_daemon *d, struct vela_bt_device *device)
{
    struct vela_bt_operation *operation = device->operation;
    if (!operation) {
        return;
    }
    // Pairing and connecting stop; Disconnect, Remove and Rename run to
    // their end.
    if (operation->step == VELA_BT_STEP_PAIR) {
        vela_bt_call(d, NULL, device->path, DEVICE1, "CancelPairing", VELA_BT_STEP_TIMEOUT_US, NULL, NULL, NULL);
    } else if (operation->step == VELA_BT_STEP_CONNECT) {
        vela_bt_call(d, NULL, device->path, DEVICE1, "Disconnect", VELA_BT_STEP_TIMEOUT_US, NULL, NULL, NULL);
    } else if (operation->step != VELA_BT_STEP_TRUST) {
        return;
    }
    vela_bt_operation_end(d, device, NULL, true);
}

void vela_bt_operations_device_gone(struct vela_bt_daemon *d, struct vela_bt_device *device)
{
    if (device->operation) {
        bool removed = device->operation->kind == VELA_BT_REMOVE;
        vela_bt_operation_end(d, device, removed ? NULL : &vela_bt_device_gone, false);
    }
}

void vela_bt_operations_adapter_off(struct vela_bt_daemon *d, const char *adapter)
{
    for (size_t i = 0; i < d->device_count; ++i) {
        struct vela_bt_device *device = d->devices[i];
        if (!device->operation || !device->adapter || strcmp(device->adapter, adapter) != 0) {
            continue;
        }
        switch (device->operation->kind) {
        case VELA_BT_ADD:
        case VELA_BT_CONNECT:
            vela_bt_operation_end(d, device, &vela_bt_adapter_off, false);
            break;
        case VELA_BT_DISCONNECT:
            vela_bt_operation_end(d, device, NULL, false);
            break;
        default:
            // Remove and Rename work with the adapter off.
            break;
        }
    }
}

bool vela_bt_operations_running(const struct vela_bt_daemon *d)
{
    for (size_t i = 0; i < d->device_count; ++i) {
        if (d->devices[i]->operation) {
            return true;
        }
    }
    return false;
}
