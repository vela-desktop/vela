// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BT_OUTCOME_H
#define VELA_BT_OUTCOME_H

// How the steps of an operation end (docs/bluetooth.md §6.3): BlueZ's
// errors become a short, stable code and a message in plain words, and an
// Add goes from step to step. No D-Bus here: plain C, unit-tested.

#include <stdbool.h>

enum vela_bt_step {
    VELA_BT_STEP_PAIR,
    VELA_BT_STEP_TRUST,
    VELA_BT_STEP_CONNECT,
    VELA_BT_STEP_DISCONNECT,
    VELA_BT_STEP_REMOVE,
    VELA_BT_STEP_RENAME,
    VELA_BT_STEP_POWER,
};

// A failure for the client: `code` is stable, `message` is for the user.
// Both are static strings.
struct vela_bt_failure {
    const char *code;
    const char *message;
};

// The failure that BlueZ's error `name` (with its `message`, which may be
// NULL) means for `step`. NULL when the error counts as success for that
// step: AlreadyExists for Pair, AlreadyConnected for Connect, NotConnected
// for Disconnect, DoesNotExist for Remove.
const struct vela_bt_failure *vela_bt_failure_of(enum vela_bt_step step, const char *name, const char *message);

// What a client sees while `step` runs: "pairing", "trusting",
// "connecting", "disconnecting", "removing", "renaming", "powering".
const char *vela_bt_step_state(enum vela_bt_step step);

// The failures the daemon reports by itself.
extern const struct vela_bt_failure vela_bt_failed; // nothing more precise
extern const struct vela_bt_failure vela_bt_device_gone; // Device1 disappeared
extern const struct vela_bt_failure vela_bt_adapter_off; // the adapter stopped being powered
extern const struct vela_bt_failure vela_bt_adapter_gone; // the adapter went away
extern const struct vela_bt_failure vela_bt_not_running; // org.bluez left the bus
extern const struct vela_bt_failure vela_bt_not_trusted; // Set Trusted failed: shown, the Add goes on
extern const struct vela_bt_failure vela_bt_paired_not_connected; // an Add's Connect failed
extern const struct vela_bt_failure vela_bt_hard_blocked; // a hardware switch blocks the radio
extern const struct vela_bt_failure vela_bt_power_on_failed;
extern const struct vela_bt_failure vela_bt_power_off_failed;
extern const struct vela_bt_failure vela_bt_no_adapter;
extern const struct vela_bt_failure vela_bt_no_response; // the device didn't answer in time
extern const struct vela_bt_failure vela_bt_pairing_cancelled; // the device cancelled the pairing
extern const struct vela_bt_failure vela_bt_session_inactive; // the user switched to another session (§6.7)
extern const struct vela_bt_failure vela_bt_session_locked; // the screen locked (§6.7)

// What an Add does once `step` has ended, `ok` or not (§6.3):
// Pair -> Trust -> Connect. Trust goes on to Connect even when it failed.
enum vela_bt_add_next {
    VELA_BT_ADD_TRUST,
    VELA_BT_ADD_CONNECT,
    VELA_BT_ADD_DONE,
    VELA_BT_ADD_FAILED,
};

enum vela_bt_add_next vela_bt_add_next(enum vela_bt_step step, bool ok);

#endif
