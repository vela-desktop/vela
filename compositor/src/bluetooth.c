// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bluetooth.h"

#include "process.h"
#include "server.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#ifdef VELA_HAVE_SYSTEMD_LOCK_HINT
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#endif

void vela_bluetooth_start(struct vela_server *server)
{
    char command[PATH_MAX + 2] = "";
    const char *custom = getenv("VELA_BLUETOOTH");
    if (custom && *custom) {
        if (strcmp(custom, "0") == 0) {
            return;
        }
        snprintf(command, sizeof(command), "%s", custom);
    } else {
        if (!server->session) {
            return;
        }
        if (!vela_find_program("vela-bluetooth", "bluetooth", command, sizeof(command))) {
            wlr_log(WLR_ERROR, "Bluetooth: vela-bluetooth not found");
            return;
        }
    }
    wlr_log(WLR_INFO, "Starting Bluetooth: %s", command);
    vela_child_start(&server->bluetooth, server->loop, command);
}

#ifdef VELA_HAVE_SYSTEMD_LOCK_HINT
static sd_bus *lock_bus;
static char *lock_session_path;
#endif

void vela_bluetooth_report_lock(struct vela_server *server, bool locked)
{
#ifndef VELA_HAVE_SYSTEMD_LOCK_HINT
    (void)server;
    (void)locked;
#else
    // Only a real DRM session reports logind hints. Nested sessions must
    // never report a lock on the host desktop.
    if (!server->session) {
        return;
    }
    if (!lock_bus) {
        int r = sd_bus_open_system(&lock_bus);
        if (r < 0) {
            wlr_log(WLR_ERROR, "Bluetooth lock hint: system bus: %s", strerror(-r));
            return;
        }
        // Do not stall the compositor if logind is unavailable.
        sd_bus_set_method_call_timeout(lock_bus, 150000);
    }
    if (!lock_session_path) {
        char *own = NULL;
        const char *id = sd_pid_get_session(0, &own) >= 0 ? own : getenv("XDG_SESSION_ID");
        if (!id || !*id) {
            free(own);
            return;
        }
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = NULL;
        int r = sd_bus_call_method(lock_bus, "org.freedesktop.login1", "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager", "GetSession", &error, &reply, "s", id);
        const char *path = NULL;
        if (r >= 0 && sd_bus_message_read(reply, "o", &path) > 0) {
            lock_session_path = strdup(path);
        }
        if (!lock_session_path) {
            wlr_log(WLR_ERROR, "Bluetooth lock hint: cannot identify logind session: %s",
                r < 0 ? strerror(-r) : "invalid reply");
        }
        sd_bus_error_free(&error);
        sd_bus_message_unref(reply);
        free(own);
        if (!lock_session_path) {
            return;
        }
    }
    // Fire-and-forget: the operation must not block frame scheduling.
    sd_bus_message *call = NULL;
    int r = sd_bus_message_new_method_call(lock_bus, &call, "org.freedesktop.login1",
        lock_session_path, "org.freedesktop.login1.Session", "SetLockedHint");
    if (r >= 0) {
        r = sd_bus_message_append(call, "b", (int)locked);
    }
    if (r >= 0) {
        r = sd_bus_send(lock_bus, call, NULL);
    }
    if (r >= 0) {
        r = sd_bus_flush(lock_bus);
    }
    if (r < 0) {
        wlr_log(WLR_ERROR, "Bluetooth lock hint: %s", strerror(-r));
    }
    sd_bus_message_unref(call);
#endif
}

void vela_bluetooth_finish(void)
{
#ifdef VELA_HAVE_SYSTEMD_LOCK_HINT
    free(lock_session_path);
    lock_session_path = NULL;
    lock_bus = sd_bus_flush_close_unref(lock_bus);
#endif
}
