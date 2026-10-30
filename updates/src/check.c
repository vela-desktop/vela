// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Checks (docs/updates.md §6.2): vela-update --check --json as a child,
// now or on schedule, and NetworkManager's word on whether the connection
// may be used for one.

#include "daemon.h"

#include "child.h"
#include "config.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_OUTPUT (1024 * 1024)
#define NM_NAME "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"

static uint64_t now_us(clockid_t clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void check_finish(struct vela_up_daemon *d)
{
    d->check_buffer = d->check_buffer ? d->check_buffer : strdup("");
    struct vela_up_check check;
    if (!d->check_buffer || vela_up_check_parse(d->check_buffer, &check) < 0) {
        // No report at all: the script couldn't even start, or crashed.
        vela_up_log("vela-update --check ended without a result (status %d)", d->check_status);
        memset(&check, 0, sizeof(check));
        check.valid = true;
        check.status = strdup("error");
        check.error_code = strdup("failed");
        check.error_message = strdup("The update check ended without a result.");
        check.checked_at = (int64_t)time(NULL);
        if (!check.status || !check.error_code || !check.error_message) {
            vela_up_check_clear(&check);
        }
    }
    bool failed = check.valid && strcmp(check.status, "error") == 0;
    bool network = failed
        && (strcmp(check.error_code, "network") == 0 || strcmp(check.error_code, "rate-limited") == 0);
    d->failures = network ? d->failures + 1 : 0;
    vela_up_adopt_check(d, &check);
    vela_up_check_clear(&check);

    free(d->check_buffer);
    d->check_buffer = NULL;
    d->check_length = 0;
    d->check_pid = 0;
    d->check_exited = false;
    d->check_child = sd_event_source_disable_unref(d->check_child);
    d->check_timeout = sd_event_source_disable_unref(d->check_timeout);
    d->check_kill = sd_event_source_disable_unref(d->check_kill);
    vela_up_state_reload(d);
    vela_up_schedule(d);
}

static void close_output(struct vela_up_daemon *d)
{
    d->check_io = sd_event_source_disable_unref(d->check_io);
    if (d->check_out >= 0) {
        close(d->check_out);
        d->check_out = -1;
    }
}

static int on_check_output(sd_event_source *source, int fd, uint32_t revents, void *data)
{
    struct vela_up_daemon *d = data;
    char chunk[8192];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        if (d->check_length + (size_t)n > MAX_OUTPUT) {
            continue; // not a report anymore; the parse will fail
        }
        char *grown = realloc(d->check_buffer, d->check_length + (size_t)n + 1);
        if (!grown) {
            continue;
        }
        d->check_buffer = grown;
        memcpy(d->check_buffer + d->check_length, chunk, (size_t)n);
        d->check_length += (size_t)n;
        d->check_buffer[d->check_length] = '\0';
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        close_output(d);
        if (d->check_exited) {
            check_finish(d);
        }
    }
    return 0;
}

static int on_check_exit(sd_event_source *source, const siginfo_t *info, void *data)
{
    struct vela_up_daemon *d = data;
    d->check_exited = true;
    d->check_status = info->si_code == CLD_EXITED ? info->si_status : 128 + info->si_status;
    if (!d->check_io) {
        check_finish(d);
    }
    return 0;
}

static int on_check_kill(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_up_daemon *d = data;
    if (d->check_pid > 0) {
        kill(d->check_pid, SIGKILL);
    }
    return 0;
}

static int on_check_timeout(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_up_daemon *d = data;
    vela_up_log("vela-update --check is taking too long: stopping it");
    if (d->check_pid > 0) {
        kill(d->check_pid, SIGTERM);
        sd_event_add_time_relative(d->event, &d->check_kill, CLOCK_MONOTONIC, d->timing.kill_us, 0, on_check_kill, d);
    }
    return 0;
}

int vela_up_check_start(struct vela_up_daemon *d)
{
    if (strcmp(vela_up_activity(d), "idle") != 0) {
        return -EBUSY;
    }
    char *argv[] = { (char *)d->command, "--check", "--json", NULL };
    int out = -1;
    pid_t pid = vela_up_spawn(argv, &out, false);
    if (pid < 0) {
        vela_up_log("can't start %s: %s", d->command, strerror((int)-pid));
        return (int)pid;
    }
    fcntl(out, F_SETFL, fcntl(out, F_GETFL) | O_NONBLOCK);
    d->check_pid = pid;
    d->check_out = out;
    d->check_exited = false;
    int r = sd_event_add_io(d->event, &d->check_io, out, EPOLLIN, on_check_output, d);
    if (r >= 0) {
        r = sd_event_add_child(d->event, &d->check_child, pid, WEXITED, on_check_exit, d);
    }
    if (r >= 0) {
        r = sd_event_add_time_relative(d->event, &d->check_timeout, CLOCK_MONOTONIC, d->timing.check_timeout_us, 0,
            on_check_timeout, d);
    }
    if (r < 0) {
        vela_up_log("following the check: %s", strerror(-r));
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close_output(d);
        d->check_pid = 0;
        d->check_child = sd_event_source_disable_unref(d->check_child);
        return r;
    }
    d->auto_timer = sd_event_source_disable_unref(d->auto_timer);
    d->waiting_for_network = false;
    d->attempt_us = now_us(CLOCK_BOOTTIME);
    vela_up_changed(d);
    return 0;
}

static bool network_allowed(const struct vela_up_daemon *d)
{
    return d->connected && !d->metered;
}

static int on_auto_timer(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_up_daemon *d = data;
    d->auto_timer = sd_event_source_disable_unref(d->auto_timer);
    if (!d->auto_check) {
        return 0;
    }
    if (!network_allowed(d)) {
        // Retried when NetworkManager says the connection may be used.
        d->waiting_for_network = true;
        return 0;
    }
    if (vela_up_check_start(d) < 0) {
        // Busy (an install runs) or broken: later.
        sd_event_add_time_relative(d->event, &d->auto_timer, CLOCK_BOOTTIME, d->timing.retry_us, 0, on_auto_timer, d);
    }
    return 0;
}

void vela_up_schedule(struct vela_up_daemon *d)
{
    d->auto_timer = sd_event_source_disable_unref(d->auto_timer);
    d->waiting_for_network = false;
    if (!d->auto_check || d->check_pid > 0) {
        return;
    }
    uint64_t wait = d->timing.interval_us;
    if (d->failures > 0) {
        unsigned shift = d->failures - 1 < 5 ? d->failures - 1 : 5;
        wait = d->timing.retry_us << shift;
        wait = wait < d->timing.interval_us ? wait : d->timing.interval_us;
    }
    uint64_t now = now_us(CLOCK_BOOTTIME);
    uint64_t delay;
    if (d->attempt_us) {
        // After our own check: on the monotonic clock, whatever the dates
        // in the reports say.
        delay = d->attempt_us + wait > now ? d->attempt_us + wait - now : 0;
    } else {
        // The first one: from the last attempt in the reports, completed or
        // not, also from earlier sessions. A date in the future (a clock
        // that was wrong) waits one interval at most.
        int64_t last = d->check.valid ? d->check.checked_at : 0;
        if (d->error_at > last) {
            last = d->error_at;
        }
        uint64_t wall = now_us(CLOCK_REALTIME);
        uint64_t due = last > 0 ? (uint64_t)last * 1000000ULL + wait : 0;
        delay = due > wall ? due - wall : 0;
        delay = delay < wait ? delay : wait;
    }
    // Never before the first check's delay after the start (§6.2).
    uint64_t since_start = now - d->started_us;
    uint64_t first = since_start < d->timing.first_check_us ? d->timing.first_check_us - since_start : 0;
    delay = delay > first ? delay : first;
    int r = sd_event_add_time_relative(d->event, &d->auto_timer, CLOCK_BOOTTIME, delay, 0, on_auto_timer, d);
    if (r < 0) {
        vela_up_log("automatic check timer: %s", strerror(-r));
    }
}

void vela_up_set_auto_check(struct vela_up_daemon *d, bool on)
{
    if (on == d->auto_check) {
        return;
    }
    d->auto_check = on;
    if (!vela_config_write_checked("update-check", on ? "yes" : "no")) {
        vela_up_log("can't save update-check in vela.conf");
    }
    vela_up_schedule(d);
    vela_up_changed(d);
}

static void network_update(struct vela_up_daemon *d, sd_bus_message *m)
{
    // a{sv}: Metered (NMMetered: 1 yes, 3 guess-yes) and Connectivity
    // (NMConnectivityState: 4 full; 0 unknown, when checking is off).
    // Unknown is not evidence of full connectivity; fail closed here.
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) {
        return;
    }
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *name = NULL;
        uint32_t value;
        if (sd_bus_message_read(m, "s", &name) < 0) {
            break;
        }
        if (strcmp(name, "Metered") == 0 && sd_bus_message_read(m, "v", "u", &value) >= 0) {
            d->metered = value == 1 || value == 3;
        } else if (strcmp(name, "Connectivity") == 0 && sd_bus_message_read(m, "v", "u", &value) >= 0) {
            d->connected = value == 4;
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    if (d->waiting_for_network && network_allowed(d)) {
        d->waiting_for_network = false;
        if (vela_up_check_start(d) < 0) {
            vela_up_schedule(d);
        }
    }
}

static int on_network_all(sd_bus_message *m, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    d->network_call = sd_bus_slot_unref(d->network_call);
    if (sd_bus_message_is_method_error(m, NULL)) {
        // No NetworkManager: nothing says the connection is metered.
        d->metered = false;
        d->connected = true;
        return 0;
    }
    network_update(d, m);
    return 0;
}

static void network_read(struct vela_up_daemon *d)
{
    d->network_call = sd_bus_slot_unref(d->network_call);
    sd_bus_call_method_async(d->system, &d->network_call, NM_NAME, NM_PATH, "org.freedesktop.DBus.Properties",
        "GetAll", on_network_all, d, "s", NM_NAME);
}

static int on_network_changed(sd_bus_message *m, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    const char *interface = NULL;
    if (sd_bus_message_read(m, "s", &interface) >= 0 && strcmp(interface, NM_NAME) == 0) {
        network_update(d, m);
    }
    return 0;
}

static int on_network_owner(sd_bus_message *m, void *data, sd_bus_error *error)
{
    struct vela_up_daemon *d = data;
    const char *name, *old_owner, *new_owner;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0) {
        return 0;
    }
    if (*new_owner) {
        network_read(d);
    } else {
        d->metered = false;
        d->connected = true;
        if (d->waiting_for_network) {
            d->waiting_for_network = false;
            vela_up_check_start(d);
        }
    }
    return 0;
}

int vela_up_network_start(struct vela_up_daemon *d)
{
    d->metered = false;
    d->connected = true;
    if (!d->system) {
        return 0;
    }
    int r = sd_bus_match_signal_async(d->system, &d->network_match, NM_NAME, NM_PATH,
        "org.freedesktop.DBus.Properties", "PropertiesChanged", on_network_changed, NULL, d);
    if (r >= 0) {
        r = sd_bus_add_match_async(d->system, &d->network_owner,
            "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
            "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" NM_NAME "'",
            on_network_owner, NULL, d);
    }
    if (r >= 0) {
        network_read(d);
    }
    return r;
}

void vela_up_check_free(struct vela_up_daemon *d)
{
    if (d->check_pid > 0) {
        kill(d->check_pid, SIGTERM);
    }
    close_output(d);
    free(d->check_buffer);
    d->check_child = sd_event_source_disable_unref(d->check_child);
    d->check_timeout = sd_event_source_disable_unref(d->check_timeout);
    d->check_kill = sd_event_source_disable_unref(d->check_kill);
    d->auto_timer = sd_event_source_disable_unref(d->auto_timer);
    d->network_match = sd_bus_slot_unref(d->network_match);
    d->network_owner = sd_bus_slot_unref(d->network_owner);
    d->network_call = sd_bus_slot_unref(d->network_call);
}
