// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-bluetooth: the only process of Vela that talks to BlueZ
// (docs/bluetooth.md §6). The compositor starts it and restarts it if it
// crashes; it serves org.vela.Bluetooth to Settings, the shell and
// vela-audio.
//
// Exit codes: 0 on SIGTERM, 1 on an error, 2 when another instance already
// owns org.vela.Bluetooth (the compositor doesn't restart it then).
//
// The test build (vela-bluetooth-testing, never installed) trusts the
// tests' UI executable instead of Vela's, and reads VELA_RFKILL and
// VELA_SYSFS in place of /dev/rfkill and /sys.

#include "daemon.h"

#include "config.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Vela's UI (§7.3): the installed vela-shell and vela-settings, and, for a
// daemon run from its build directory, the ones built next to it.
static const char *const *ui_executables(void)
{
#ifdef VELA_BLUETOOTH_TESTING
    static const char *const testing[] = { VELA_BT_TEST_UI, NULL };
    return testing;
#else
    static const char *const installed[] = { VELA_BT_SHELL, VELA_BT_SETTINGS, NULL };
    static const char *const built[] = { VELA_BT_SHELL, VELA_BT_SETTINGS, VELA_BT_BUILD_SHELL, VELA_BT_BUILD_SETTINGS,
        NULL };
    char self[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length > 0) {
        self[length] = '\0';
        if (strcmp(self, VELA_BT_BUILD_SELF) == 0) {
            return built;
        }
    }
    return installed;
#endif
}

static const char *test_path(const char *variable, const char *fallback)
{
#ifdef VELA_BLUETOOTH_TESTING
    const char *path = secure_getenv(variable);
    if (path && *path) {
        return path;
    }
#endif
    return fallback;
}

static int on_signal(sd_event_source *source, const struct signalfd_siginfo *info, void *data)
{
    struct vela_bt_daemon *d = data;
    // A pairing in progress stops on the device too.
    if (d->pairing) {
        vela_bt_operation_cancel(d, d->pairing);
    }
    return sd_event_exit(sd_event_source_get_event(source), 0);
}

int main(void)
{
    static struct vela_bt_daemon d;
    d.sysfs = test_path("VELA_SYSFS", "/sys");
    d.rfkill = test_path("VELA_RFKILL", "/dev/rfkill");
    d.ui = ui_executables();

    struct vela_config settings;
    vela_config_read(&settings);
    const char *chosen = vela_config_get(&settings, "bluetooth-adapter", "");
    d.chosen = *chosen ? strdup(chosen) : NULL;
    vela_config_finish(&settings);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    signal(SIGPIPE, SIG_IGN);

    int r = sd_event_default(&d.event);
    if (r >= 0) {
        r = sd_event_add_signal(d.event, NULL, SIGTERM, on_signal, &d);
    }
    if (r >= 0) {
        r = sd_event_add_signal(d.event, NULL, SIGINT, on_signal, &d);
    }
    if (r < 0) {
        vela_bt_log("event loop: %s", strerror(-r));
        return 1;
    }
    if ((r = sd_bus_open_system(&d.system)) < 0 || (r = sd_bus_attach_event(d.system, d.event, 0)) < 0) {
        vela_bt_log("system bus: %s", strerror(-r));
        return 1;
    }
    if ((r = sd_bus_open_user(&d.session)) < 0 || (r = sd_bus_attach_event(d.session, d.event, 0)) < 0) {
        vela_bt_log("session bus: %s", strerror(-r));
        return 1;
    }
    r = vela_bt_service_start(&d);
    if (r == -EEXIST || r == -EALREADY) {
        vela_bt_log("another instance owns " VELA_BT_NAME);
        return 2;
    }
    if (r < 0) {
        vela_bt_log(VELA_BT_NAME ": %s", strerror(-r));
        return 1;
    }
    if ((r = vela_bt_bluez_start(&d)) < 0) {
        vela_bt_log("following BlueZ: %s", strerror(-r));
        return 1;
    }
    vela_bt_rfkill_watch(&d);
    vela_bt_platform_refresh(&d);
    vela_bt_flush(&d);

    r = sd_event_loop(d.event);
    // CancelPairing and the replies go out before leaving.
    sd_bus_flush(d.system);
    sd_bus_flush(d.session);
    vela_bt_clear(&d);
    vela_bt_model_free(&d);
    sd_event_source_disable_unref(d.flush);
    sd_event_source_disable_unref(d.rfkill_watch);
    sd_bus_flush_close_unref(d.system);
    sd_bus_flush_close_unref(d.session);
    sd_event_unref(d.event);
    return r < 0 ? 1 : 0;
}
