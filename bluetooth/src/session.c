// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The logind session (docs/bluetooth.md §6.7). The daemon is a child of
// the compositor, so it's in the compositor's session, found as the polkit
// agent finds it; XDG_SESSION_ID if logind can't name it. Vela acts only
// while that session is active, and the agent is registered only then.
// While the screen is locked (LockedHint), an Add stops and requests the
// user did not start are refused.
//
// Without a session, or without logind, the daemon acts as if the session
// were always in front, as Bluetooth did before.

#include "daemon.h"

#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-login.h>

#define LOGIN1 "org.freedesktop.login1"
#define SESSION1 "org.freedesktop.login1.Session"

bool vela_bt_active(const struct vela_bt_daemon *d)
{
    return d->session_active && !d->session_locked;
}

// What Vela started stops: the question closes with `reason`, and a running
// Add fails with `failure`.
static void stop(struct vela_bt_daemon *d, const char *reason, const struct vela_bt_failure *failure)
{
    vela_bt_question_close(d, NULL, reason);
    if (d->pairing) {
        vela_bt_operation_cancel(d, d->pairing, failure);
    }
}

static void apply(struct vela_bt_daemon *d, bool active, bool locked)
{
    bool was_active = d->session_active;
    bool was_locked = d->session_locked;
    bool was_in_front = vela_bt_active(d);
    d->session_active = active;
    d->session_locked = locked;
    if (active != was_active || locked != was_locked) {
        vela_bt_log("session %s%s", active ? "active" : "in the background", locked ? ", locked" : "");
    }
    if (was_active && !active) {
        stop(d, "inactive", &vela_bt_session_inactive);
    } else if (active && locked && !was_locked) {
        stop(d, "locked", &vela_bt_session_locked);
    }
    vela_bt_agent_update(d);
    if (vela_bt_active(d) != was_in_front) {
        vela_bt_properties_changed(d);
    }
}

// Active and LockedHint from an a{sv} at the message's position.
static int read_state(struct vela_bt_daemon *d, sd_bus_message *m)
{
    bool active = d->session_active;
    bool locked = d->session_locked;
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    while (r >= 0) {
        r = sd_bus_message_enter_container(m, 'e', "sv");
        if (r <= 0) {
            break;
        }
        const char *name;
        int value;
        if ((r = sd_bus_message_read_basic(m, 's', &name)) < 0) {
            break;
        }
        if (strcmp(name, "Active") != 0 && strcmp(name, "LockedHint") != 0) {
            r = sd_bus_message_skip(m, "v");
            if (r < 0) {
                break;
            }
        } else if (sd_bus_message_read(m, "v", "b", &value) > 0) {
            *(strcmp(name, "Active") == 0 ? &active : &locked) = value != 0;
        }
        r = sd_bus_message_exit_container(m);
    }
    if (r < 0) {
        return r;
    }
    sd_bus_message_exit_container(m);
    apply(d, active, locked);
    return 0;
}

static int on_changed(sd_bus_message *m, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *interface;
    if (sd_bus_message_read_basic(m, 's', &interface) > 0 && strcmp(interface, SESSION1) == 0) {
        read_state(d, m);
    }
    return 0;
}

static int on_properties(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    d->session_call = sd_bus_slot_unref(d->session_call);
    if (sd_bus_message_is_method_error(reply, NULL) || read_state(d, reply) < 0) {
        vela_bt_log("logind session: %s: acting as if it's in front",
            sd_bus_message_is_method_error(reply, NULL) ? sd_bus_message_get_error(reply)->message : "bad reply");
        apply(d, true, false);
    }
    return 0;
}

static int on_session(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    d->session_call = sd_bus_slot_unref(d->session_call);
    const char *path;
    if (!sd_bus_message_is_method_error(reply, NULL) && sd_bus_message_read(reply, "o", &path) > 0) {
        d->login_session = strdup(path);
    }
    if (!d->login_session) {
        vela_bt_log("logind session: %s: acting as if it's in front",
            sd_bus_message_is_method_error(reply, NULL) ? sd_bus_message_get_error(reply)->message : "bad reply");
        apply(d, true, false);
        return 0;
    }
    // The match goes to the bus before GetAll: no change is missed.
    int r = sd_bus_match_signal_async(d->system, NULL, LOGIN1, d->login_session, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", on_changed, NULL, d);
    if (r >= 0) {
        r = sd_bus_call_method_async(d->system, &d->session_call, LOGIN1, d->login_session,
            "org.freedesktop.DBus.Properties", "GetAll", on_properties, d, "s", SESSION1);
    }
    if (r < 0) {
        vela_bt_log("logind session %s: %s: acting as if it's in front", d->login_session, strerror(-r));
        apply(d, true, false);
    }
    return 0;
}

int vela_bt_session_start(struct vela_bt_daemon *d)
{
    char *own = NULL;
    const char *id = sd_pid_get_session(0, &own) >= 0 ? own : getenv("XDG_SESSION_ID");
    int r = -ENXIO;
    if (id && *id) {
        r = sd_bus_call_method_async(d->system, &d->session_call, LOGIN1, "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager", "GetSession", on_session, d, "s", id);
    }
    if (r < 0) {
        vela_bt_log("no logind session: acting as if it's in front");
        apply(d, true, false);
    }
    free(own);
    return 0;
}
