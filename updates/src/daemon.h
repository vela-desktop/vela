// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_UP_DAEMON_H
#define VELA_UP_DAEMON_H

// vela-update-daemon (docs/updates.md §6): what it knows and the event
// loop's sources. One per session.

#include "report.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#define VELA_UP_NAME "org.vela.Updates"
#define VELA_UP_PATH "/org/vela/Updates"
#define VELA_UP_ERROR(name) VELA_UP_NAME ".Error." name

// Timings (§6.2, §6.3), shorter in the tests' build.
struct vela_up_timing {
    uint64_t first_check_us; // after the daemon starts
    uint64_t interval_us; // between automatic checks
    uint64_t retry_us; // first retry after a network error, doubling
    uint64_t check_timeout_us; // a check that takes longer is stopped
    uint64_t start_timeout_us; // the terminal has this long to start the update
};

struct vela_up_daemon {
    sd_event *event;
    sd_bus *session;
    sd_bus *system; // NetworkManager; NULL without a system bus
    struct vela_up_timing timing;
    const char *command; // vela-update
    const char *terminal; // a test's terminal, or NULL: found at Install
    char *state_dir; // $XDG_STATE_HOME/vela-update
    char *config_dir; // $XDG_CONFIG_HOME/vela
    pid_t session_pid; // the compositor whose executable an update replaces

    // What the state files say (§5.3).
    struct vela_up_check check; // the last completed check
    char *error_code; // the last check's failure, or NULL
    char *error_message;
    int64_t error_at;
    int64_t file_seen_at; // checked_at of the last check.json read
    struct vela_up_run run; // running now, by anyone
    sd_event_source *run_watch; // its pidfd
    struct vela_up_install install; // the last install's end
    char *source; // Experimental's source (updates.conf)

    // Our own check (§6.2).
    pid_t check_pid;
    int check_out; // its stdout
    char *check_buffer;
    size_t check_length;
    bool check_exited;
    int check_status;
    sd_event_source *check_child;
    sd_event_source *check_io;
    sd_event_source *check_timeout;
    sd_event_source *check_kill;

    // An install from Install(), until vela-update starts (§6.3).
    bool starting;
    int64_t starting_at;
    sd_event_source *start_timeout; // then it ends as failed, "terminal"

    // Automatic checks (§6.2).
    uint64_t started_us; // CLOCK_BOOTTIME when the daemon started
    uint64_t attempt_us; // CLOCK_BOOTTIME of our last check, 0 if none yet
    bool auto_check;
    unsigned failures; // network errors in a row
    sd_event_source *auto_timer;
    bool waiting_for_network; // due, but the connection is metered or down
    bool metered;
    bool connected;

    char *announced; // the last update announced (§6.4)
    bool needs_relogin;

    int inotify;
    sd_event_source *inotify_io;
    sd_event_source *flush; // PropertiesChanged, once per loop iteration
    sd_bus_slot *network_match;
    sd_bus_slot *network_owner;
    sd_bus_slot *network_call;
};

// service.c: org.vela.Updates (§7).
int vela_up_service_start(struct vela_up_daemon *d);
// Sends PropertiesChanged soon (all properties: they are few and small).
void vela_up_changed(struct vela_up_daemon *d);
const char *vela_up_activity(const struct vela_up_daemon *d);
// The commit to tell the user about (§6.4), or "".
const char *vela_up_announce(const struct vela_up_daemon *d);

// state.c: the state files, followed with inotify.
int vela_up_state_start(struct vela_up_daemon *d);
void vela_up_state_reload(struct vela_up_daemon *d);
// A check's outcome (check.json, or our own check's output); takes it.
void vela_up_adopt_check(struct vela_up_daemon *d, struct vela_up_check *check);
void vela_up_announced(struct vela_up_daemon *d, const char *commit);
void vela_up_relogin_refresh(struct vela_up_daemon *d);
void vela_up_state_free(struct vela_up_daemon *d);

// check.c: running checks, now and on schedule.
// 0, -EBUSY if a check or an install runs, or another negative errno.
int vela_up_check_start(struct vela_up_daemon *d);
void vela_up_schedule(struct vela_up_daemon *d);
void vela_up_set_auto_check(struct vela_up_daemon *d, bool on);
int vela_up_network_start(struct vela_up_daemon *d);
void vela_up_check_free(struct vela_up_daemon *d);

// install.c: Install() (§6.3). 0, or a negative errno with `error` set.
int vela_up_install_start(struct vela_up_daemon *d, const struct vela_up_options *options, sd_bus_error *error);
// The run started, or ended, or the wait is over: no longer starting.
void vela_up_install_started(struct vela_up_daemon *d);
void vela_up_install_free(struct vela_up_daemon *d);

#endif
