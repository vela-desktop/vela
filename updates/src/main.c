// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-update-daemon: checks for updates in the background and follows
// vela-update for Settings and the shell (docs/updates.md §6). The
// compositor starts it and restarts it if it crashes; it serves
// org.vela.Updates.
//
// Exit codes: 0 on SIGTERM, 1 on an error, 2 when another instance already
// owns org.vela.Updates (the compositor doesn't restart it then).
//
// The test build (vela-update-daemon-testing, never installed) also reads
// VELA_UPDATE_COMMAND (vela-update), VELA_UPDATE_TERMINAL (a program that
// gets the command), VELA_UPDATE_SESSION_PID (the process whose replaced
// executable means "sign out") and shorter timings in milliseconds.

#include "daemon.h"

#include "config.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef VELA_UPDATE_TESTING
static const char *test_value(const char *variable)
{
    const char *value = secure_getenv(variable);
    return value && *value ? value : NULL;
}
#endif

static uint64_t milliseconds(const char *variable, uint64_t fallback_us)
{
#ifdef VELA_UPDATE_TESTING
    const char *ms = test_value(variable);
    if (ms) {
        return strtoull(ms, NULL, 10) * 1000ULL;
    }
#endif
    return fallback_us;
}

// vela-update: the installed one, or, for a daemon run from its build
// directory, the one in the source tree next to it.
static const char *command(void)
{
#ifdef VELA_UPDATE_TESTING
    const char *custom = test_value("VELA_UPDATE_COMMAND");
    if (custom) {
        return custom;
    }
#endif
    char self[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length > 0) {
        self[length] = '\0';
        if (strcmp(self, VELA_UP_BUILD_SELF) == 0) {
            return VELA_UP_SOURCE_COMMAND;
        }
    }
    return VELA_UP_COMMAND;
}

// $XDG_<kind>_HOME/<name>, or ~/<fallback>/<name>.
static char *user_dir(const char *variable, const char *fallback, const char *name)
{
    const char *base = getenv(variable);
    char *path = NULL;
    int r = base && *base == '/' ? asprintf(&path, "%s/%s", base, name)
                                 : asprintf(&path, "%s/%s/%s", getenv("HOME") ? getenv("HOME") : "", fallback, name);
    return r < 0 ? NULL : path;
}

static uint64_t boottime_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static int on_signal(sd_event_source *source, const struct signalfd_siginfo *info, void *data)
{
    return sd_event_exit(sd_event_source_get_event(source), 0);
}

int main(void)
{
    static struct vela_up_daemon d;
    d.inotify = -1;
    d.check_out = -1;
    d.command = command();
    d.timing = (struct vela_up_timing) {
        .first_check_us = milliseconds("VELA_UPDATE_FIRST_CHECK_MS", 10 * 60 * 1000000ULL),
        .interval_us = milliseconds("VELA_UPDATE_INTERVAL_MS", 24 * 3600 * 1000000ULL),
        .retry_us = milliseconds("VELA_UPDATE_RETRY_MS", 3600 * 1000000ULL),
        .check_timeout_us = milliseconds("VELA_UPDATE_CHECK_TIMEOUT_MS", 15 * 60 * 1000000ULL),
        .kill_us = milliseconds("VELA_UPDATE_KILL_MS", 10 * 1000000ULL),
        .start_timeout_us = milliseconds("VELA_UPDATE_START_TIMEOUT_MS", 60 * 1000000ULL),
    };
    d.started_us = boottime_us();
    d.state_dir = user_dir("XDG_STATE_HOME", ".local/state", "vela-update");
    d.config_dir = user_dir("XDG_CONFIG_HOME", ".config", "vela");
    if (!d.state_dir || !d.config_dir) {
        return 1;
    }
    d.session_pid = vela_up_session_process("/proc", getppid());
#ifdef VELA_UPDATE_TESTING
    d.terminal = test_value("VELA_UPDATE_TERMINAL");
    if (test_value("VELA_UPDATE_SESSION_PID")) {
        d.session_pid = (pid_t)strtol(test_value("VELA_UPDATE_SESSION_PID"), NULL, 10);
    }
#endif

    struct vela_config settings;
    vela_config_read(&settings);
    d.auto_check = vela_config_flag(&settings, "update-check", true);
    const char *interval = vela_config_get(&settings, "update-check-interval", "24");
    char *end = NULL;
    unsigned long hours = strtoul(interval, &end, 10);
    d.check_interval_hours = *interval && *end == '\0' && hours <= UINT32_MAX
        && vela_up_valid_check_interval((uint32_t)hours) ? (uint32_t)hours : 24;
    vela_config_finish(&settings);
    d.timing.interval_us = milliseconds("VELA_UPDATE_INTERVAL_MS", (uint64_t)d.check_interval_hours * 3600 * 1000000ULL);

    // SIGCHLD for the children's sources (sd_event_add_child).
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGCHLD);
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
        vela_up_log("event loop: %s", strerror(-r));
        return 1;
    }
    r = sd_bus_open_user(&d.session);
    if (r >= 0) {
        r = sd_bus_attach_event(d.session, d.event, 0);
    }
    if (r < 0) {
        vela_up_log("session bus: %s", strerror(-r));
        return 1;
    }
    // Only for NetworkManager: without a system bus, every connection may
    // be used.
    if (sd_bus_open_system(&d.system) < 0 || sd_bus_attach_event(d.system, d.event, 0) < 0) {
        d.system = sd_bus_flush_close_unref(d.system);
    }
    r = vela_up_service_start(&d);
    if (r == -EEXIST || r == -EALREADY) {
        vela_up_log("another instance owns " VELA_UP_NAME);
        return 2;
    }
    if (r < 0) {
        vela_up_log(VELA_UP_NAME ": %s", strerror(-r));
        return 1;
    }
    if ((r = vela_up_state_start(&d)) < 0) {
        vela_up_log("following %s: %s", d.state_dir, strerror(-r));
        return 1;
    }
    if ((r = vela_up_network_start(&d)) < 0) {
        vela_up_log("following NetworkManager: %s", strerror(-r));
    }
    vela_up_schedule(&d);

    r = sd_event_loop(d.event);
    vela_up_check_free(&d);
    vela_up_install_free(&d);
    vela_up_state_free(&d);
    sd_event_source_disable_unref(d.flush);
    sd_bus_flush_close_unref(d.system);
    sd_bus_flush_close_unref(d.session);
    sd_event_unref(d.event);
    free(d.state_dir);
    free(d.config_dir);
    return r < 0 ? 1 : 0;
}
