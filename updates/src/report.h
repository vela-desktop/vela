// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_UP_REPORT_H
#define VELA_UP_REPORT_H

// The pure parts of vela-update-daemon, covered by unit tests: what
// vela-update reports (docs/updates.md §5), Install's options, the
// terminal, and whether the session must restart.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct vela_up_pair {
    char *first; // a commit, or a version
    char *second; // its summary, or the release's text
};

// check.json, or --check --json's output (§5.1). Strings are never NULL
// once parsed ("" when missing).
struct vela_up_check {
    bool valid;
    char *channel;
    char *status;
    char *installed_package;
    char *installed_version;
    char *version;
    char *tag;
    char *commit;
    char *source;
    struct vela_up_pair *changes;
    size_t change_count;
    uint32_t changes_total;
    struct vela_up_pair *notes;
    size_t note_count;
    int64_t checked_at;
    char *error_code; // with status "error"
    char *error_message;
};

// run.json (§5.3).
struct vela_up_run {
    bool present;
    char *action; // "check", "install"
    char *channel;
    char *phase;
    pid_t pid;
    uint64_t start_time; // clock ticks since boot, /proc/PID/stat's 22nd field
    int64_t started_at;
};

// install.json (§5.3).
struct vela_up_install {
    bool present;
    char *channel;
    char *result; // "success", "failed", "cancelled"
    char *error_code;
    char *error_message;
    char *installed_version;
    int64_t finished_at;
};

// 0, or a negative errno (-EINVAL: not schema 2, or not an object).
int vela_up_check_parse(const char *json, struct vela_up_check *check);
// As parse, from a file; -ENOENT if there's none.
int vela_up_check_read(const char *path, struct vela_up_check *check);
void vela_up_check_clear(struct vela_up_check *check);
int vela_up_run_read(const char *path, struct vela_up_run *run);
void vela_up_run_clear(struct vela_up_run *run);
int vela_up_install_read(const char *path, struct vela_up_install *install);
void vela_up_install_clear(struct vela_up_install *install);

// The run's process still exists: same PID and same start time. `proc` is
// /proc, or the tests' fake.
bool vela_up_run_alive(const char *proc, const struct vela_up_run *run);
// A process's start time (clock ticks since boot), or 0.
uint64_t vela_up_start_time(const char *proc, pid_t pid);

// Install's options (§6.3); NULL when not given.
struct vela_up_options {
    const char *channel;
    const char *branch;
    const char *commit;
    const char *source;
    const char *tag;
};

// NULL if vela-update would accept the options, else why not. `installed`:
// the installed channel, when known, for options that depend on it.
const char *vela_up_options_invalid(const struct vela_up_options *options, const char *installed);
// vela-update's arguments for the options, after `command`, in `argv`
// (NULL-terminated; 12 entries are enough). The strings stay the
// options', but for "--tag=...", written in `tag`.
void vela_up_options_argv(const struct vela_up_options *options, const char *command, const char **argv,
    char tag[64]);

// The terminal that runs a command given after it (§4): xdg-terminal-exec,
// else `vela_terminal` (VELA_TERMINAL, words split on spaces) then -e,
// else konsole -e, each looked up in `path` (PATH). A NULL-terminated,
// allocated list (vela_up_strv_free), or NULL when there's none.
char **vela_up_terminal(const char *path, const char *vela_terminal);
void vela_up_strv_free(char **strv);

// The session's compositor: from `pid` upwards, the first process whose
// executable is named vela-compositor. 0 if there's none.
pid_t vela_up_session_process(const char *proc, pid_t pid);
// The process's executable was replaced or removed since it started, as a
// package upgrade does.
bool vela_up_exe_replaced(const char *proc, pid_t pid);

#endif
