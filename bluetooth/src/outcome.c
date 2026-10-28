// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "outcome.h"

#include <stddef.h>
#include <string.h>

const struct vela_bt_failure vela_bt_no_response
    = { "no-response", "The device isn't responding. Make sure it's on and nearby." };
static const struct vela_bt_failure rejected = { "rejected", "Pairing was refused on the device." };
const struct vela_bt_failure vela_bt_pairing_cancelled
    = { "pairing-cancelled", "Pairing was cancelled on the device." };
static const struct vela_bt_failure auth_failed = { "auth-failed", "Pairing didn't work. Check the code and try again." };
static const struct vela_bt_failure busy = { "busy", "Bluetooth is busy. Try again in a moment." };
static const struct vela_bt_failure not_supported
    = { "not-supported", "This PC can't use any of this device's features." };
static const struct vela_bt_failure forgotten
    = { "forgotten", "The device no longer knows this PC. Remove it, then add it again." };
const struct vela_bt_failure vela_bt_failed = { "failed", "Something went wrong. Try again." };

const struct vela_bt_failure vela_bt_device_gone = { "device-gone", "The device is no longer available." };
const struct vela_bt_failure vela_bt_adapter_off = { "adapter-off", "Bluetooth is turned off." };
const struct vela_bt_failure vela_bt_adapter_gone = { "adapter-gone", "The Bluetooth adapter was removed." };
const struct vela_bt_failure vela_bt_not_running = { "not-running", "Bluetooth isn't running." };
const struct vela_bt_failure vela_bt_not_trusted
    = { "not-trusted", "Paired, but the device couldn't be marked as trusted. It may ask again before it connects." };
const struct vela_bt_failure vela_bt_paired_not_connected = { "paired-not-connected", "Paired, but couldn't connect." };
const struct vela_bt_failure vela_bt_hard_blocked
    = { "hard-blocked", "Bluetooth is turned off by a switch or key on this PC." };
const struct vela_bt_failure vela_bt_power_on_failed = { "power-failed", "Bluetooth couldn't be turned on." };
const struct vela_bt_failure vela_bt_power_off_failed = { "power-failed", "Bluetooth couldn't be turned off." };
const struct vela_bt_failure vela_bt_no_adapter = { "no-adapter", "Bluetooth isn't available on this PC." };
const struct vela_bt_failure vela_bt_session_inactive
    = { "inactive", "Stopped because you switched to another session." };
const struct vela_bt_failure vela_bt_session_locked = { "locked", "Stopped because the screen locked." };

static bool starts_with(const char *text, const char *prefix)
{
    return text && strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool contains(const char *text, const char *part)
{
    return text && strstr(text, part);
}

// BlueZ 5.70 and later explain a failed connection in the message:
// "br-connection-page-timeout", "le-connection-abort-by-remote"...
// (btd_error_bredr_errno and btd_error_le_errno in src/error.c).
static const struct vela_bt_failure *connection_reason(const char *message)
{
    if (!starts_with(message, "br-connection-") && !starts_with(message, "le-connection-")) {
        return NULL;
    }
    if (contains(message, "timeout") || contains(message, "by-remote")) {
        return &vela_bt_no_response;
    }
    if (contains(message, "profile-unavailable") || contains(message, "not-supported")) {
        return &not_supported;
    }
    if (contains(message, "not-powered")) {
        return &vela_bt_adapter_off;
    }
    if (contains(message, "busy") || contains(message, "concurrent-connection-limit")) {
        return &busy;
    }
    if (contains(message, "key-missing")) {
        return &forgotten;
    }
    return &vela_bt_failed;
}

const struct vela_bt_failure *vela_bt_failure_of(enum vela_bt_step step, const char *name, const char *message)
{
    static const char bluez[] = "org.bluez.Error.";
    static const char dbus[] = "org.freedesktop.DBus.Error.";
    const char *error = starts_with(name, bluez) ? name + strlen(bluez) : "";
    const char *dbus_error = starts_with(name, dbus) ? name + strlen(dbus) : "";

    if ((step == VELA_BT_STEP_PAIR && strcmp(error, "AlreadyExists") == 0)
        || (step == VELA_BT_STEP_CONNECT && strcmp(error, "AlreadyConnected") == 0)
        || (step == VELA_BT_STEP_CONNECT && contains(message, "-connection-already-connected"))
        || (step == VELA_BT_STEP_DISCONNECT && strcmp(error, "NotConnected") == 0)
        || (step == VELA_BT_STEP_REMOVE && strcmp(error, "DoesNotExist") == 0)) {
        return NULL;
    }

    const struct vela_bt_failure *failure = &vela_bt_failed;
    if (strcmp(dbus_error, "ServiceUnknown") == 0 || strcmp(dbus_error, "NameHasNoOwner") == 0
        || strcmp(dbus_error, "Disconnected") == 0) {
        failure = &vela_bt_not_running;
    } else if (strcmp(dbus_error, "NoReply") == 0 || strcmp(dbus_error, "Timeout") == 0
        || strcmp(dbus_error, "TimedOut") == 0) {
        failure = &vela_bt_no_response;
    } else if (strcmp(dbus_error, "UnknownObject") == 0 || strcmp(error, "DoesNotExist") == 0) {
        failure = &vela_bt_device_gone;
    } else if (strcmp(error, "AuthenticationRejected") == 0 || strcmp(error, "Rejected") == 0) {
        failure = &rejected;
    } else if (strcmp(error, "AuthenticationCanceled") == 0 || strcmp(error, "Canceled") == 0) {
        failure = &vela_bt_pairing_cancelled;
    } else if (strcmp(error, "AuthenticationFailed") == 0) {
        failure = &auth_failed;
    } else if (strcmp(error, "AuthenticationTimeout") == 0) {
        failure = &vela_bt_no_response;
    } else if (strcmp(error, "InProgress") == 0 || strcmp(error, "Busy") == 0) {
        failure = &busy;
    } else if (strcmp(error, "Blocked") == 0) {
        failure = &vela_bt_hard_blocked;
    } else if (strcmp(error, "NotReady") == 0) {
        failure = &vela_bt_adapter_off;
    } else if (strcmp(error, "NotSupported") == 0 || strcmp(error, "NotAvailable") == 0) {
        failure = &not_supported;
    } else if (strcmp(error, "ConnectionAttemptFailed") == 0 || strcmp(error, "Failed") == 0) {
        const struct vela_bt_failure *reason = connection_reason(message);
        if (reason) {
            failure = reason;
        } else if (strcmp(error, "ConnectionAttemptFailed") == 0 || contains(message, "Page Timeout")
            || contains(message, "Host is down")) {
            failure = &vela_bt_no_response;
        } else if (contains(message, "already in progress")) {
            failure = &busy;
        } else if (contains(message, "rfkill") || contains(message, "Blocked")) {
            failure = &vela_bt_hard_blocked;
        }
    }
    if (step == VELA_BT_STEP_POWER && failure == &vela_bt_failed) {
        failure = &vela_bt_power_on_failed;
    }
    return failure;
}

enum vela_bt_add_next vela_bt_add_next(enum vela_bt_step step, bool ok)
{
    switch (step) {
    case VELA_BT_STEP_PAIR:
        return ok ? VELA_BT_ADD_TRUST : VELA_BT_ADD_FAILED;
    case VELA_BT_STEP_TRUST:
        return VELA_BT_ADD_CONNECT;
    case VELA_BT_STEP_CONNECT:
        return ok ? VELA_BT_ADD_DONE : VELA_BT_ADD_FAILED;
    default:
        return VELA_BT_ADD_FAILED;
    }
}

const char *vela_bt_step_state(enum vela_bt_step step)
{
    static const char *const states[] = {
        [VELA_BT_STEP_PAIR] = "pairing",
        [VELA_BT_STEP_TRUST] = "trusting",
        [VELA_BT_STEP_CONNECT] = "connecting",
        [VELA_BT_STEP_DISCONNECT] = "disconnecting",
        [VELA_BT_STEP_REMOVE] = "removing",
        [VELA_BT_STEP_RENAME] = "renaming",
        [VELA_BT_STEP_POWER] = "powering",
    };
    return (unsigned)step < sizeof(states) / sizeof(states[0]) ? states[step] : "";
}
