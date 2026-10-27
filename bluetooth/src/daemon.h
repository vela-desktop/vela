// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BT_DAEMON_H
#define VELA_BT_DAEMON_H

// vela-bluetooth's state (docs/bluetooth.md §6): BlueZ's adapters and
// devices as the daemon follows them on the system bus, the operations it
// runs, and org.vela.Bluetooth on the session bus. One event loop
// (sd-event), no threads.

#include "outcome.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#define VELA_BT_NAME "org.vela.Bluetooth"
#define VELA_BT_PATH "/org/vela/Bluetooth"
#define VELA_BT_ERROR(name) "org.vela.Bluetooth.Error." name

// Longer than BlueZ's own, so BlueZ's error normally arrives first: it gives
// an agent 60 s per question (REQUEST_TIMEOUT in src/agent.c).
#define VELA_BT_PAIR_TIMEOUT_US (90 * 1000000ULL)
#define VELA_BT_STEP_TIMEOUT_US (30 * 1000000ULL)

// Callers waiting for an operation, or a power change, to end.
struct vela_bt_waiters {
    sd_bus_message **calls;
    size_t count;
};

enum vela_bt_kind {
    VELA_BT_ADD,
    VELA_BT_CONNECT,
    VELA_BT_DISCONNECT,
    VELA_BT_REMOVE,
    VELA_BT_RENAME,
};

struct vela_bt_operation {
    struct vela_bt_daemon *daemon;
    struct vela_bt_device *device;
    enum vela_bt_kind kind;
    enum vela_bt_step step;
    sd_bus_slot *call; // BlueZ's call in flight
    sd_event_source *timer; // Remove: waiting for Device1 to go
    struct vela_bt_waiters waiters;
    char *name; // Rename
};

struct vela_bt_device {
    char *path;
    char *adapter; // its adapter's path
    char *address;
    char *name; // BlueZ's Alias
    char *icon;
    uint32_t class_of_device;
    bool paired;
    bool trusted;
    bool connected;
    bool blocked;
    bool has_rssi;
    int16_t rssi;
    int battery; // percent, -1: none
    struct vela_bt_operation *operation;
    const struct vela_bt_failure *failure; // the last operation's, or NULL
    bool changed; // goes out in the next DevicesChanged
    bool seen; // while GetManagedObjects is read
};

struct vela_bt_adapter {
    char *path;
    char *address;
    char *name;
    char *power_state;
    bool powered;
    bool discoverable;
    bool discovering;
    bool seen;
};

// SetPowered in progress (§6.3).
struct vela_bt_power {
    bool active;
    bool on;
    int attempt;
    char *adapter;
    sd_bus_slot *call;
    sd_event_source *timer;
    struct vela_bt_waiters waiters;
};

struct vela_bt_daemon {
    sd_event *event;
    sd_bus *system;
    sd_bus *session;
    const char *sysfs; // /sys, or the tests' fake
    const char *rfkill; // /dev/rfkill, or the tests' fake
    const char *const *ui; // Vela's UI executables (§7.3)

    char *bluez; // org.bluez's unique name, NULL while it isn't on the bus
    sd_bus_slot *managed; // GetManagedObjects in flight
    struct vela_bt_adapter **adapters; // by hci number
    size_t adapter_count;
    struct vela_bt_device **devices;
    size_t device_count;
    char *in_use; // the adapter in use, or NULL
    char *chosen; // the address the user chose (vela.conf), or NULL
    struct vela_bt_device *pairing; // the device whose Add runs

    char **removed; // devices for the next DevicesChanged
    size_t removed_count;
    bool properties_changed;
    sd_event_source *flush;

    bool platform_blocked;
    sd_event_source *rfkill_watch;
    struct vela_bt_power power;
    uint64_t power_on_until; // CLOCK_MONOTONIC µs: power the adapter that appears
};

// model.c: adapters and devices.
struct vela_bt_adapter *vela_bt_adapter_find(struct vela_bt_daemon *d, const char *path);
struct vela_bt_adapter *vela_bt_adapter_in_use(struct vela_bt_daemon *d);
struct vela_bt_device *vela_bt_device_find(struct vela_bt_daemon *d, const char *path);
bool vela_bt_device_listed(struct vela_bt_daemon *d, const struct vela_bt_device *device);
bool vela_bt_available(const struct vela_bt_daemon *d);
// Reads one object of GetManagedObjects or InterfacesAdded: an
// a{sa{sv}} at the message's position.
int vela_bt_read_object(struct vela_bt_daemon *d, const char *path, sd_bus_message *m);
int vela_bt_read_changes(struct vela_bt_daemon *d, const char *path, sd_bus_message *m);
int vela_bt_read_removal(struct vela_bt_daemon *d, const char *path, sd_bus_message *m);
void vela_bt_remove_device(struct vela_bt_daemon *d, struct vela_bt_device *device);
void vela_bt_remove_adapter(struct vela_bt_daemon *d, struct vela_bt_adapter *adapter);
// org.bluez left: every adapter and device goes.
void vela_bt_clear(struct vela_bt_daemon *d);
// Picks the adapter in use again (§6.2).
void vela_bt_choose_adapter(struct vela_bt_daemon *d);
void vela_bt_touch(struct vela_bt_daemon *d, struct vela_bt_device *device);
void vela_bt_properties_changed(struct vela_bt_daemon *d);
void vela_bt_append_device(sd_bus_message *m, const struct vela_bt_device *device);
void vela_bt_append_adapter(sd_bus_message *m, const struct vela_bt_adapter *adapter);
void vela_bt_model_free(struct vela_bt_daemon *d);

// bluez.c: the system bus side.
int vela_bt_bluez_start(struct vela_bt_daemon *d);
// A call to org.bluez; `slot` NULL: nobody waits for the reply.
int vela_bt_call(struct vela_bt_daemon *d, sd_bus_slot **slot, const char *path, const char *interface,
    const char *member, uint64_t timeout, sd_bus_message_handler_t reply, void *data, const char *types, ...);

// operations.c (§6.3).
int vela_bt_operation_start(struct vela_bt_daemon *d, sd_bus_message *call, struct vela_bt_device *device,
    enum vela_bt_kind kind, const char *name, sd_bus_error *error);
// Ends the device's operation: done (`failure` NULL), failed, or cancelled.
void vela_bt_operation_end(
    struct vela_bt_daemon *d, struct vela_bt_device *device, const struct vela_bt_failure *failure, bool cancelled);
void vela_bt_operation_cancel(struct vela_bt_daemon *d, struct vela_bt_device *device);
// The model's events that end operations early.
void vela_bt_operations_device_gone(struct vela_bt_daemon *d, struct vela_bt_device *device);
void vela_bt_operations_adapter_off(struct vela_bt_daemon *d, const char *adapter);
bool vela_bt_operations_running(const struct vela_bt_daemon *d);

// power.c: SetPowered and rfkill (§6.3).
int vela_bt_power_set(struct vela_bt_daemon *d, sd_bus_message *call, bool on, sd_bus_error *error);
void vela_bt_power_adapter_changed(struct vela_bt_daemon *d);
void vela_bt_power_end(struct vela_bt_daemon *d, const struct vela_bt_failure *failure, bool cancelled);
void vela_bt_platform_refresh(struct vela_bt_daemon *d);
int vela_bt_rfkill_watch(struct vela_bt_daemon *d);

// service.c: org.vela.Bluetooth (§7).
int vela_bt_service_start(struct vela_bt_daemon *d);
// Sends DevicesChanged and PropertiesChanged for what changed.
void vela_bt_flush(struct vela_bt_daemon *d);
void vela_bt_emit_operation(struct vela_bt_daemon *d, const struct vela_bt_device *device, const char *state,
    const struct vela_bt_failure *failure);

// Waiting callers.
void vela_bt_waiters_add(struct vela_bt_waiters *waiters, sd_bus_message *call);
// Replies to each: done, cancelled (Error.Cancelled), or failed
// (Error.Failed with the failure's message).
void vela_bt_waiters_reply(struct vela_bt_waiters *waiters, const struct vela_bt_failure *failure, bool cancelled);

uint64_t vela_bt_now(void);

#endif
