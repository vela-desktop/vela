// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Operations on devices (docs/bluetooth.md §6.3): one per device, one Add
// at a time (§6.5). Each ends in done, failed or cancelled, also when the
// device, the adapter or BlueZ goes away, or the session goes to the
// background.

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
    sd_bus_track_unref(operation->prompters);
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
    // A question left open goes with its Add.
    if (operation->kind == VELA_BT_ADD) {
        vela_bt_question_close(d, device->path, "cancelled-by-device");
    }
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
        __attribute__((fallthrough));
    case VELA_BT_ADD_CONNECT:
        if (!device->connected) {
            run_step(d, device, VELA_BT_STEP_CONNECT);
            return;
        }
        __attribute__((fallthrough));
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
        // A passkey or PIN shown for typing on the device isn't needed any
        // more.
        if (operation->step == VELA_BT_STEP_PAIR) {
            vela_bt_question_close(d, device->path, ok ? "answered" : "cancelled-by-device");
        }
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
        if (sd_bus_message_is_method_error(reply, "org.bluez.Error.DoesNotExist")) {
            // Already removed on BlueZ: there will be no InterfacesRemoved event.
            // Removing the model entry also completes the pending operation.
            vela_bt_remove_device(d, device);
        } else if (!ok) {
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

static int on_prompters_gone(sd_bus_track *track, void *data)
{
    struct vela_bt_operation *operation = data;
    // sd-bus calls a handler that returns 0 again before anything else on
    // the bus: the empty track goes now.
    operation->prompters = sd_bus_track_unref(operation->prompters);
    // The shell draws the question now (§7.2).
    if (vela_bt_question_on(operation->daemon, operation->device->path)) {
        vela_bt_question_resend(operation->daemon);
    }
    return 1;
}

static void add_prompter(struct vela_bt_daemon *d, struct vela_bt_operation *operation, sd_bus_message *call)
{
    if (!operation->prompters
        && sd_bus_track_new(d->session, &operation->prompters, on_prompters_gone, operation) < 0) {
        return;
    }
    sd_bus_track_add_sender(operation->prompters, call);
    vela_bt_listen(d, call);
}

static int on_adopted_timeout(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_bt_operation *operation = data;
    vela_bt_operation_end(operation->daemon, operation->device, &vela_bt_no_response, false);
    return 0;
}

// An adopted Add goes on once the device is paired and its question
// answered.
static void adopted_continue(struct vela_bt_daemon *d, struct vela_bt_device *device)
{
    struct vela_bt_operation *operation = device->operation;
    if (operation && operation->adopted && operation->step == VELA_BT_STEP_PAIR && device->paired
        && !vela_bt_question_on(d, device->path)) {
        operation->timer = sd_event_source_disable_unref(operation->timer);
        add_continue(d, device, VELA_BT_ADD_TRUST, NULL);
    }
}

int vela_bt_operation_start(struct vela_bt_daemon *d, sd_bus_message *call, struct vela_bt_device *device,
    enum vela_bt_kind kind, const char *name, bool prompts_here, sd_bus_error *error)
{
    struct vela_bt_operation *running = device->operation;
    if (running) {
        // The same operation again joins it; anything else waits (§6.5).
        if (running->kind == kind && (kind != VELA_BT_RENAME || strcmp(running->name, name) == 0)) {
            vela_bt_waiters_add(&running->waiters, call);
            if (prompts_here) {
                add_prompter(d, running, call);
                if (vela_bt_question_on(d, device->path)) {
                    vela_bt_question_resend(d);
                }
            }
            return 1;
        }
        return sd_bus_error_set(error, VELA_BT_ERROR("Busy"), device->path);
    }
    if (kind == VELA_BT_ADD && d->pairing && d->pairing != device) {
        return sd_bus_error_set(error, VELA_BT_ERROR("Busy"), d->pairing->path);
    }
    // A device asking by itself blocks the other Adds: BlueZ would refuse
    // them anyway. An Add on that device takes its request over (§6.5).
    struct vela_bt_question *asking = vela_bt_question_on(d, NULL);
    if (kind == VELA_BT_ADD && asking && asking->unsolicited && strcmp(asking->device, device->path) != 0) {
        return sd_bus_error_set(error, VELA_BT_ERROR("Busy"), asking->device);
    }
    bool adopt = kind == VELA_BT_ADD && asking && asking->unsolicited;
    struct vela_bt_operation *operation = calloc(1, sizeof(*operation));
    if (operation && name) {
        operation->name = strdup(name);
    }
    if (!operation || (name && !operation->name)) {
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
        if (prompts_here) {
            add_prompter(d, operation, call);
        }
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
    if (adopt) {
        // No Pair: the device's request is answered, then it's paired.
        operation->adopted = true;
        operation->step = VELA_BT_STEP_PAIR;
        asking->unsolicited = false;
        vela_bt_log("%s: Add takes over its %s", device->path, vela_bt_question_kind_name(asking->kind));
        vela_bt_touch(d, device);
        vela_bt_emit_operation(d, device, vela_bt_step_state(operation->step), NULL);
        vela_bt_question_resend(d);
        sd_event_add_time_relative(d->event, &operation->timer, CLOCK_MONOTONIC, VELA_BT_PAIR_TIMEOUT_US, 0,
            on_adopted_timeout, operation);
        return 1;
    }
    run_step(d, device, first[kind]);
    return 1;
}

void vela_bt_operation_cancel(
    struct vela_bt_daemon *d, struct vela_bt_device *device, const struct vela_bt_failure *failure)
{
    struct vela_bt_operation *operation = device->operation;
    if (!operation) {
        return;
    }
    // Pairing and connecting stop; Disconnect, Remove and Rename run to
    // their end. An adopted Add never called Pair: refusing the device's
    // request is enough.
    if (operation->step == VELA_BT_STEP_PAIR) {
        if (!operation->adopted) {
            vela_bt_call(d, NULL, device->path, DEVICE1, "CancelPairing", VELA_BT_STEP_TIMEOUT_US, NULL, NULL, NULL);
        }
    } else if (operation->step == VELA_BT_STEP_CONNECT) {
        vela_bt_call(d, NULL, device->path, DEVICE1, "Disconnect", VELA_BT_STEP_TIMEOUT_US, NULL, NULL, NULL);
    } else if (operation->step != VELA_BT_STEP_TRUST) {
        return;
    }
    if (operation->kind == VELA_BT_ADD) {
        // Not an answer that lets an adopted Add go on.
        operation->adopted = false;
        vela_bt_question_close(d, device->path, "answered");
    }
    vela_bt_operation_end(d, device, failure, !failure);
}

void vela_bt_operations_device_gone(struct vela_bt_daemon *d, struct vela_bt_device *device)
{
    if (device->operation) {
        bool removed = device->operation->kind == VELA_BT_REMOVE;
        vela_bt_operation_end(d, device, removed ? NULL : &vela_bt_device_gone, false);
    }
}

void vela_bt_operations_device_changed(struct vela_bt_daemon *d, struct vela_bt_device *device)
{
    // Paired: a passkey or PIN shown for typing isn't needed any more.
    struct vela_bt_question *question = vela_bt_question_on(d, device->path);
    if (question && device->paired && (question->kind == VELA_BT_SHOW_PASSKEY || question->kind == VELA_BT_SHOW_PIN)) {
        vela_bt_question_close(d, device->path, "answered");
    }
    adopted_continue(d, device);
}

void vela_bt_operations_question_closed(struct vela_bt_daemon *d, struct vela_bt_device *device, const char *reason)
{
    struct vela_bt_operation *operation = device->operation;
    if (!operation || !operation->adopted || operation->step != VELA_BT_STEP_PAIR) {
        return;
    }
    // Device gone, BlueZ gone, the session in the background: those end
    // the Add themselves.
    if (strcmp(reason, "cancelled-by-device") == 0) {
        vela_bt_operation_end(d, device, &vela_bt_pairing_cancelled, false);
    } else if (strcmp(reason, "answered") == 0) {
        adopted_continue(d, device);
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
