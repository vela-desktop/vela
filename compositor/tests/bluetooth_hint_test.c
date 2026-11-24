// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bluetooth.h"
#include "server.h"

#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>

static int starts;

bool vela_find_program(const char *name, const char *dir, char *command, size_t size)
{
    abort(); // the fixture supplies a command
}

bool vela_child_start(struct vela_child *child, struct wl_event_loop *loop, const char *command)
{
    assert(strcmp(command, "test-bluetooth") == 0);
    ++starts;
    return true;
}

int main(int argc, char **argv)
{
    assert(argc == 2 && *argv[1]);
    assert(setenv("DBUS_SYSTEM_BUS_ADDRESS", argv[1], 1) == 0);
    assert(setenv("VELA_BLUETOOTH", "test-bluetooth", 1) == 0);
    assert(setenv("XDG_SESSION_ID", "27", 1) == 0);
    sd_bus *logind = NULL;
    assert(sd_bus_open_system(&logind) >= 0);
    assert(sd_bus_request_name(logind, "org.freedesktop.login1", 0) >= 0);

    char *own = NULL, *path = NULL;
    const char *id = sd_pid_get_session(0, &own) >= 0 ? own : "27";
    assert(sd_bus_path_encode("/org/freedesktop/login1/session", id, &path) >= 0);
    free(own);

    struct vela_server server = { .locked = true, .loop = wl_event_loop_create() };
    assert(server.loop);
    vela_bluetooth_report_lock(&server, false); // nested: sends nothing
    server.session = (struct wlr_session *)&server;
    vela_bluetooth_start(&server);
    server.locked = false;
    vela_bluetooth_report_lock(&server, false);

    int hints = 0;
    while (hints != 2) {
        assert(wl_event_loop_dispatch(server.loop, 10) >= 0);
        sd_bus_message *m = NULL;
        int r;
        while ((r = sd_bus_process(logind, &m)) > 0) {
            if (m && sd_bus_message_is_method_call(m, "org.freedesktop.login1.Session", "SetLockedHint")) {
                int locked;
                assert(hints < 2 && strcmp(sd_bus_message_get_path(m), path) == 0);
                assert(sd_bus_message_read(m, "b", &locked) == 1 && locked == (hints == 0));
                assert(sd_bus_message_get_expect_reply(m) == 0);
                ++hints;
            }
            m = sd_bus_message_unref(m);
        }
        assert(r >= 0);
    }
    assert(starts == 1);
    vela_bluetooth_finish();
    wl_event_loop_destroy(server.loop);
    sd_bus_close_unref(logind);
    free(path);
    return 0;
}
