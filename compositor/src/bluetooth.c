// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bluetooth.h"

#include "process.h"
#include "server.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#ifdef VELA_HAVE_LOGIND
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#endif

static void start_daemon(struct vela_server *server)
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

#ifdef VELA_HAVE_LOGIND
static sd_bus *lock_bus;
static char *lock_session_path;
static struct wl_event_source *bus_source;
static struct wl_event_source *write_timer;
static bool start_pending;

static void stop_dispatch(void)
{
    if (bus_source) {
        wl_event_source_remove(bus_source);
        bus_source = NULL;
    }
    if (write_timer) {
        wl_event_source_remove(write_timer);
        write_timer = NULL;
    }
}

static void finish_hint(struct vela_server *server, int result)
{
    stop_dispatch();
    if (result < 0) {
        wlr_log(WLR_ERROR, "Bluetooth lock hint: %s", strerror(-result));
        lock_bus = sd_bus_close_unref(lock_bus);
    }
    if (start_pending && (result >= 0 || !server->locked)) {
        start_pending = false;
        start_daemon(server);
    }
}

static int drain_hint(void *data);

static int bus_ready(int fd, uint32_t mask, void *data)
{
    return drain_hint(data);
}

static int watch_bus(struct vela_server *server, bool more_work)
{
    uint64_t deadline;
    int r = sd_bus_get_timeout(lock_bus, &deadline);
    if (r < 0) {
        return r;
    }
    int events = sd_bus_get_events(lock_bus);
    if (events < 0) {
        return events;
    }
    uint32_t mask = (events & POLLIN ? WL_EVENT_READABLE : 0) | (events & POLLOUT ? WL_EVENT_WRITABLE : 0);
    if (!bus_source) {
        int fd = sd_bus_get_fd(lock_bus);
        if (fd < 0) {
            return fd;
        }
        bus_source = wl_event_loop_add_fd(server->loop, fd, mask, bus_ready, server);
        if (!bus_source) {
            return -errno;
        }
    } else if (wl_event_source_fd_update(bus_source, mask) < 0) {
        return -errno;
    }
    int delay = 0; // zero disarms a Wayland timer
    if (more_work) {
        delay = 1;
    } else if (deadline != UINT64_MAX) {
        uint64_t now = vela_now_ns() / 1000;
        uint64_t ms = deadline > now ? (deadline - now) / 1000 + 1 : 1;
        delay = ms > INT_MAX ? INT_MAX : (int)ms;
    }
    if (delay && !write_timer) {
        write_timer = wl_event_loop_add_timer(server->loop, drain_hint, server);
        if (!write_timer) {
            return -errno;
        }
    }
    return write_timer && wl_event_source_timer_update(write_timer, delay) < 0 ? -errno : 0;
}

// sd_bus_send normally writes immediately. Drive authentication and queued
// writes from Wayland readiness only when the socket actually needs it.
static int drain_hint(void *data)
{
    struct vela_server *server = data;
    int r;
    for (int i = 0;; ++i) {
        uint64_t queued;
        r = sd_bus_get_n_queued_write(lock_bus, &queued);
        if (r < 0 || !queued) {
            break;
        }
        r = sd_bus_process(lock_bus, NULL);
        if (r < 0) {
            break;
        }
        if (r == 0 || i == 15) {
            r = watch_bus(server, r > 0);
            if (r >= 0) {
                return 0;
            }
            break;
        }
    }
    finish_hint(server, r);
    return 0;
}

static int send_hint(bool locked)
{
    if (!lock_session_path) {
        char *own = NULL;
        const char *id = sd_pid_get_session(0, &own) >= 0 ? own : getenv("XDG_SESSION_ID");
        int r = id && *id ? sd_bus_path_encode("/org/freedesktop/login1/session", id, &lock_session_path) : -ENXIO;
        free(own);
        if (r < 0) {
            return r;
        }
    }
    if (!lock_bus) {
        int r = sd_bus_open_system(&lock_bus);
        if (r < 0) {
            return r;
        }
    }
    sd_bus_message *call = NULL;
    int r = sd_bus_message_new_method_call(lock_bus, &call, "org.freedesktop.login1", lock_session_path,
        "org.freedesktop.login1.Session", "SetLockedHint");
    if (r >= 0) {
        r = sd_bus_message_append(call, "b", (int)locked);
    }
    if (r >= 0) {
        r = sd_bus_send(lock_bus, call, NULL);
    }
    sd_bus_message_unref(call);
    return r;
}
#endif

void vela_bluetooth_report_lock(struct vela_server *server, bool locked)
{
#ifdef VELA_HAVE_LOGIND
    if (server->session) {
        int r = send_hint(locked);
        if (r < 0) {
            finish_hint(server, r);
        } else {
            drain_hint(server);
        }
    }
#endif
}

void vela_bluetooth_start(struct vela_server *server)
{
#ifdef VELA_HAVE_LOGIND
    if (server->session) {
        // Queue the initial hint before the daemon can accept any requests.
        start_pending = true;
        vela_bluetooth_report_lock(server, server->locked);
        return;
    }
#endif
    start_daemon(server);
}

void vela_bluetooth_finish(void)
{
#ifdef VELA_HAVE_LOGIND
    stop_dispatch();
    start_pending = false;
    free(lock_session_path);
    lock_session_path = NULL;
    lock_bus = sd_bus_close_unref(lock_bus);
#endif
}
