// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Install() (docs/updates.md §6.3): vela-update in a terminal, where the
// package manager asks for the password and the user sees the build. The
// terminal is never followed: some hand the command to an instance already
// running and return at once. What vela-update writes in its state
// directory is followed instead.

#include "daemon.h"

#include "alloc/alloc.h"
#include "child.h"
#include "log.h"
#include "terminal.h"

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
    d->install.channel = vela_strdup("");
    d->install.result = vela_strdup("failed");
    d->install.error_code = vela_strdup("terminal");
    d->install.error_message = vela_strdup("The terminal did not start the update.");
    d->install.installed_version = vela_strdup("");
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
    const char *update[12];
    char tag[64];
    vela_up_options_argv(options, d->command, update, tag);
    // sh -c SCRIPT sh UPDATE..., in the terminal.
    const char *command[4 + 12 + 1];
    size_t n = 0;
    command[n++] = "sh";
    command[n++] = "-c";
    command[n++] = script;
    command[n++] = "sh";
    for (size_t i = 0; update[i]; ++i) {
        command[n++] = update[i];
    }
    command[n] = NULL;
    char **argv = NULL;
    if (d->terminal) {
        // A test's terminal takes the command as it is.
        argv = VELA_NEW_N(char *, n + 2);
        argv[0] = vela_strdup(d->terminal);
        for (size_t i = 0; i < n; ++i) {
            argv[i + 1] = vela_strdup(command[i]);
        }
    } else {
        // The terminal of docs/keyboard.md §11, as Win+Enter opens it.
        struct vela_terminal_env env;
        vela_terminal_env_get(&env);
        struct vela_terminal terminal;
        if (vela_terminal_find(&env, false, &terminal)) {
            argv = vela_terminal_argv(&terminal, command);
            vela_terminal_free(&terminal);
        }
    }
    if (!argv) {
        return sd_bus_error_set(error, VELA_UP_ERROR("NoTerminal"), "No terminal is installed.");
    }
    pid_t pid = vela_up_spawn(argv, NULL, VELA_UP_SPAWN_DETACH);
    vela_terminal_strv_free(argv);
    if (pid < 0) {
        vela_up_log("can't open the terminal: %s", strerror((int)-pid));
        return sd_bus_error_set(error, VELA_UP_ERROR("NoTerminal"), "The terminal could not be opened.");
    }
    // Owned by the loop (floating): only there to reap the terminal.
    int r = sd_event_add_child(d->event, NULL, pid, WEXITED, on_terminal_exit, d);
    if (r < 0) {
        vela_up_log("terminal watch: %s (it is reaped when the daemon exits)", strerror(-r));
    }

    // "Starting" ends when vela-update's run file appears (state.c) or when
    // this timeout fires. Without the timeout it could stay forever, so it
    // is not entered: the activity stays "idle" and Install still succeeds.
    // That is a trade-off. An install that does start is followed from its
    // run file as usual; one whose terminal never starts vela-update reports
    // nothing. Failing it here would report a failure for every install
    // that succeeds, and nothing else tells "never started" from "not yet":
    // the terminal's exit doesn't, since some terminals hand the command to
    // an instance already running and return at once.
    d->start_timeout = sd_event_source_disable_unref(d->start_timeout);
    r = sd_event_add_time_relative(d->event, &d->start_timeout, CLOCK_MONOTONIC, d->timing.start_timeout_us, 0,
        on_start_timeout, d);
    if (r < 0) {
        vela_up_log("start timer: %s", strerror(-r));
        return 0;
    }
    d->starting = true;
    d->starting_at = (int64_t)time(NULL);
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
