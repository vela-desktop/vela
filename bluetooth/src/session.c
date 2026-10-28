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

// Never infer an active session from a partial or malformed GetAll.
static int read_state(struct vela_bt_daemon *d, sd_bus_message *m, bool complete)
{
    bool active = d->session_active;
    bool locked = d->session_locked;
    bool have_active = false;
    bool have_locked = false;
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r <= 0) {
        return r < 0 ? r : -EBADMSG;
    }
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *name;
        if ((r = sd_bus_message_read_basic(m, 's', &name)) <= 0) {
            return r < 0 ? r : -EBADMSG;
        }
        if (strcmp(name, "Active") != 0 && strcmp(name, "LockedHint") != 0) {
            r = sd_bus_message_skip(m, "v");
        } else {
            int value = 0;
            r = sd_bus_message_read(m, "v", "b", &value);
            if (r > 0) {
                if (strcmp(name, "Active") == 0) {
                    active = value != 0;
                    have_active = true;
                } else {
                    locked = value != 0;
                    have_locked = true;
                }
            }
        }
        if (r <= 0) {
            return r < 0 ? r : -EBADMSG;
        }
        if ((r = sd_bus_message_exit_container(m)) < 0) {
            return r;
        }
    }
    if (r < 0) {
        return r;
    }
    if ((r = sd_bus_message_exit_container(m)) < 0) {
        return r;
    }
    if (complete && (!have_active || !have_locked)) {
        return -EBADMSG;
    }
    if (complete) {
        d->session_known = true;
    }
    if (d->session_known) {
        apply(d, active, locked);
    }
    return 0;
}

static int on_changed(sd_bus_message *m, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    const char *interface;
    if (sd_bus_message_read_basic(m, 's', &interface) > 0 && strcmp(interface, SESSION1) == 0) {
        if (read_state(d, m, false) < 0) {
            vela_bt_log("logind PropertiesChanged: invalid state");
        }
    }
    return 0;
}

static void request_session_state(struct vela_bt_daemon *d);

static int on_session_retry(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_bt_daemon *d = data;
    d->session_retry = sd_event_source_disable_unref(d->session_retry);
    request_session_state(d);
    return 0;
}

static void retry_session(struct vela_bt_daemon *d)
{
    if (d->session_retry || !d->login_session) {
        return;
    }
    uint64_t delay = d->session_backoff_us ? d->session_backoff_us : 1000000ULL;
    d->session_backoff_us = delay < 15000000ULL ? delay * 2 : 30000000ULL;
    int r = sd_event_add_time_relative(d->event, &d->session_retry, CLOCK_MONOTONIC,
        delay, 0, on_session_retry, d);
    if (r < 0) {
        vela_bt_log("logind retry timer: %s", strerror(-r));
    }
}

static int on_properties(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    d->session_call = sd_bus_slot_unref(d->session_call);
    if (sd_bus_message_is_method_error(reply, NULL) || read_state(d, reply, true) < 0) {
        vela_bt_log("logind session: %s: keeping session inactive until verified",
            sd_bus_message_is_method_error(reply, NULL) ? sd_bus_message_get_error(reply)->message : "bad reply");
        d->session_known = false;
        apply(d, false, true);
        retry_session(d);
    } else {
        d->session_backoff_us = 0;
    }
    return 0;
}

static void request_session_state(struct vela_bt_daemon *d)
{
    if (!d->login_session || d->session_call) {
        return;
    }
    int r = 0;
    if (!d->session_watching) {
        r = sd_bus_match_signal_async(d->system, NULL, LOGIN1, d->login_session,
            "org.freedesktop.DBus.Properties", "PropertiesChanged", on_changed, NULL, d);
        if (r >= 0) {
            d->session_watching = true;
        }
    }
    if (r >= 0) {
        r = sd_bus_call_method_async(d->system, &d->session_call, LOGIN1, d->login_session,
            "org.freedesktop.DBus.Properties", "GetAll", on_properties, d, "s", SESSION1);
    }
    if (r < 0) {
        vela_bt_log("logind session %s: %s: keeping inactive", d->login_session, strerror(-r));
        apply(d, false, true);
        retry_session(d);
    }
}

static int on_session(sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct vela_bt_daemon *d = data;
    d->session_call = sd_bus_slot_unref(d->session_call);
    const char *path = NULL;
    if (!sd_bus_message_is_method_error(reply, NULL) && sd_bus_message_read(reply, "o", &path) > 0) {
        d->login_session = strdup(path);
    }
    if (!d->login_session) {
        vela_bt_log("no identified logind session: acting as if it's in front");
        apply(d, true, false);
        return 0;
    }
    // Subscribe before fetching state, so no transitions are lost.
    request_session_state(d);
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
