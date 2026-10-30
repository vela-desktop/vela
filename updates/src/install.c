// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Install() (docs/updates.md §6.3): vela-update in a terminal, where the
// package manager asks for the password and the user sees the build. The
// terminal is never followed: some hand the command to an instance already
// running and return at once. What vela-update writes in its state
// directory is followed instead.

#include "daemon.h"

#include "child.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

// The command, then a pause so the terminal stays open on the result.
static const char script[] = "\"$@\"; s=$?; echo; "
                             "if [ \"$s\" = 0 ]; then echo 'vela-update finished.'; "
                             "else echo \"vela-update failed (exit $s).\"; fi; "
                             "printf 'Press Enter to close. '; read -r _";

static int on_terminal_exit(sd_event_source *source, const siginfo_t *info, void *data)
{
    return 0; // reaped; nothing else to know from it
}

static int on_start_timeout(sd_event_source *source, uint64_t usec, void *data)
{
    struct vela_up_daemon *d = data;
    d->start_timeout = sd_event_source_disable_unref(d->start_timeout);
    if (!d->starting) {
        return 0;
    }
    vela_up_log("the terminal did not start vela-update");
    d->starting = false;
    vela_up_install_clear(&d->install);
    d->install.present = true;
    d->install.channel = strdup("");
    d->install.result = strdup("failed");
    d->install.error_code = strdup("terminal");
    d->install.error_message = strdup("The terminal did not start the update.");
    d->install.installed_version = strdup("");
    d->install.finished_at = (int64_t)time(NULL);
    vela_up_changed(d);
    return 0;
}

int vela_up_install_start(struct vela_up_daemon *d, const struct vela_up_options *options, sd_bus_error *error)
{
    if (strcmp(vela_up_activity(d), "idle") != 0) {
        return sd_bus_error_set(error, VELA_UP_ERROR("Busy"), "An update or a check is already running.");
    }
    const char *invalid = vela_up_options_invalid(options, d->check.valid ? d->check.channel : NULL);
    if (invalid) {
        return sd_bus_error_set(error, VELA_UP_ERROR("InvalidArgs"), invalid);
    }
    char **terminal = NULL;
    if (d->terminal) {
        terminal = calloc(2, sizeof(*terminal));
        if (terminal) {
            terminal[0] = strdup(d->terminal);
        }
    } else {
        terminal = vela_up_terminal(getenv("PATH"), getenv("VELA_TERMINAL"));
    }
    if (!terminal || !terminal[0]) {
        vela_up_strv_free(terminal);
        return sd_bus_error_set(error, VELA_UP_ERROR("NoTerminal"), "No terminal is installed.");
    }

    const char *update[12];
    char tag[64];
    vela_up_options_argv(options, d->command, update, tag);
    size_t count = 0;
    while (terminal[count]) {
        ++count;
    }
    const char **argv = calloc(count + 4 + 12 + 1, sizeof(*argv));
    if (!argv) {
        vela_up_strv_free(terminal);
        return -ENOMEM;
    }
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        argv[n++] = terminal[i];
    }
    argv[n++] = "sh";
    argv[n++] = "-c";
    argv[n++] = script;
    argv[n++] = "sh";
    for (size_t i = 0; update[i]; ++i) {
        argv[n++] = update[i];
    }
    argv[n] = NULL;
    pid_t pid = vela_up_spawn((char *const *)argv, NULL, true);
    free(argv);
    vela_up_strv_free(terminal);
    if (pid < 0) {
        vela_up_log("can't open the terminal: %s", strerror((int)-pid));
        return sd_bus_error_set(error, VELA_UP_ERROR("NoTerminal"), "The terminal could not be opened.");
    }
    // Owned by the loop (floating): only there to reap the terminal.
    sd_event_add_child(d->event, NULL, pid, WEXITED, on_terminal_exit, d);

    d->starting = true;
    d->starting_at = (int64_t)time(NULL);
    d->start_timeout = sd_event_source_disable_unref(d->start_timeout);
    sd_event_add_time_relative(d->event, &d->start_timeout, CLOCK_MONOTONIC, d->timing.start_timeout_us, 0,
        on_start_timeout, d);
    vela_up_changed(d);
    return 0;
}

void vela_up_install_started(struct vela_up_daemon *d)
{
    d->starting = false;
    d->start_timeout = sd_event_source_disable_unref(d->start_timeout);
    vela_up_changed(d);
}

void vela_up_install_free(struct vela_up_daemon *d)
{
    d->start_timeout = sd_event_source_disable_unref(d->start_timeout);
}
