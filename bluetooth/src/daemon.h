// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BT_DAEMON_H
#define VELA_BT_DAEMON_H

// vela-bluetooth's state (docs/bluetooth.md §6): BlueZ's adapters and
// devices as the daemon follows them on the system bus, the operations it
// runs, its agent and the question it asks, the logind session, and
// org.vela.Bluetooth on the session bus. One event loop (sd-event), no
// threads.

#include "outcome.h"
#include "question.h"

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
    // Remove: waiting for Device1 to go. An adopted Add: waiting for Paired.
    sd_event_source *timer;
    struct vela_bt_waiters waiters;
    char *name; // Rename
    // Add: the callers that draw its questions (promptsHere, §7.2).
    sd_bus_track *prompters;
    // Add: it took over the device's own pairing request instead of calling
    // Pair (§6.5, rule 4).
    bool adopted;
};

// The one open Agent1 request (§6.4, §7.2).
struct vela_bt_question {
    uint64_t id;
    enum vela_bt_question_kind kind;
    // The device, as it was at the time of the request.
    char *device;
    char *name;
    char *icon;
    char *address;
    bool unsolicited; // no Add of the user's behind it
    uint64_t expires; // CLOCK_MONOTONIC µs
    uint32_t passkey;
    uint16_t entered;
    char pin[17];
    char *uuid;
    // BlueZ's call, waiting for the answer. NULL for show-passkey and
    // show-pin, which BlueZ doesn't wait for, and once replied.
    sd_bus_message *request;
    sd_event_source *timer; // expires
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
    sd_event_source *managed_retry;
    uint64_t managed_backoff_us;
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
    sd_event_source *rfkill_retry;
    struct vela_bt_power power;
    uint64_t power_on_until; // CLOCK_MONOTONIC µs: power the adapter that appears

    // The agent (§6.4) and its question (§7.2).
    bool agent_registered;
    bool agent_default;
    sd_event_source *agent_retry;
    uint64_t agent_backoff_us;
    sd_bus_slot *agent_call; // RegisterAgent, RequestDefaultAgent or UnregisterAgent in flight
    struct vela_bt_question *question;
    uint64_t question_id; // the last one given
    sd_bus_track *listeners; // Vela's UI clients the question signals go to

    // The logind session (§6.7).
    char *login_session; // its object path, while it's known
    bool session_active;
    bool session_locked;
    sd_bus_slot *session_call;
    sd_event_source *session_retry;
    uint64_t session_backoff_us;
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
// One a{sv} entry: `name` with a variant of `type` holding the value.
void vela_bt_append_entry(sd_bus_message *m, const char *name, const char *type, ...);
void vela_bt_append_device(sd_bus_message *m, const struct vela_bt_device *device);
void vela_bt_append_adapter(sd_bus_message *m, const struct vela_bt_adapter *adapter);
void vela_bt_model_free(struct vela_bt_daemon *d);

// bluez.c: the system bus side.
int vela_bt_bluez_start(struct vela_bt_daemon *d);
// A call to org.bluez; `slot` NULL: nobody waits for the reply.
int vela_bt_call(struct vela_bt_daemon *d, sd_bus_slot **slot, const char *path, const char *interface,
    const char *member, uint64_t timeout, sd_bus_message_handler_t reply, void *data, const char *types, ...);

// operations.c (§6.3, §6.5). `prompts_here`: the caller of an Add draws
// its questions.
int vela_bt_operation_start(struct vela_bt_daemon *d, sd_bus_message *call, struct vela_bt_device *device,
    enum vela_bt_kind kind, const char *name, bool prompts_here, sd_bus_error *error);
// Ends the device's operation: done (`failure` NULL), failed, or cancelled.
void vela_bt_operation_end(
    struct vela_bt_daemon *d, struct vela_bt_device *device, const struct vela_bt_failure *failure, bool cancelled);
// Stops pairing and connecting; Disconnect, Remove and Rename run to their
// end. `failure` NULL: the user's own Cancel, which ends it cancelled;
// otherwise it fails with `failure` (the session went to the background).
void vela_bt_operation_cancel(
    struct vela_bt_daemon *d, struct vela_bt_device *device, const struct vela_bt_failure *failure);
// The model's events that end operations early, or let them go on.
void vela_bt_operations_device_gone(struct vela_bt_daemon *d, struct vela_bt_device *device);
void vela_bt_operations_device_changed(struct vela_bt_daemon *d, struct vela_bt_device *device);
void vela_bt_operations_adapter_off(struct vela_bt_daemon *d, const char *adapter);
// The question on `device` closed with `reason` (§7.2).
void vela_bt_operations_question_closed(struct vela_bt_daemon *d, struct vela_bt_device *device, const char *reason);
bool vela_bt_operations_running(const struct vela_bt_daemon *d);

// agent.c: the agent and its question (§6.4, §7.2).
int vela_bt_agent_start(struct vela_bt_daemon *d);
// Registers the agent while BlueZ is on the bus and the session is active,
// and unregisters it otherwise (§6.7).
void vela_bt_agent_update(struct vela_bt_daemon *d);
// The open question, if its device is `device` (any device if NULL).
struct vela_bt_question *vela_bt_question_on(struct vela_bt_daemon *d, const char *device);
// Closes the open question on `device` (any device if NULL) with
// `reason`, replying Canceled to BlueZ if it still waits.
void vela_bt_question_close(struct vela_bt_daemon *d, const char *device, const char *reason);
// Sends the open question again, with the same id: whether it's
// unsolicited, or who draws it, changed.
void vela_bt_question_resend(struct vela_bt_daemon *d);
// `call`'s sender receives the question signals from now on: it's Vela's UI.
void vela_bt_listen(struct vela_bt_daemon *d, sd_bus_message *call);
int vela_bt_questions(struct vela_bt_daemon *d, sd_bus_message *call);
int vela_bt_answer(struct vela_bt_daemon *d, sd_bus_message *call, uint64_t id, const char *response,
    const char *value, sd_bus_error *error);
void vela_bt_agent_free(struct vela_bt_daemon *d);

// session.c: the logind session (§6.7).
int vela_bt_session_start(struct vela_bt_daemon *d);
// The session is in front: active and not locked.
bool vela_bt_active(const struct vela_bt_daemon *d);

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
