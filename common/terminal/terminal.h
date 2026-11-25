// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The terminal (docs/keyboard.md §11): one lookup for Win+Enter
// (vela-terminal), the updater and Settings' Default apps > Terminal.
//
// 1. the user's choice: the first entry of
//    $XDG_CONFIG_HOME/vela-xdg-terminals.list (Settings writes it);
// 2. VELA_TERMINAL, a command with options;
// 3. the system default through xdg-terminal-exec. That launcher handles
// desktop files, profile actions and terminal-specific command arguments.
#ifndef VELA_TERMINAL_H
#define VELA_TERMINAL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the lookup reads, so that tests can give their own. NULL: unset.
struct vela_terminal_env {
    const char *home; // HOME
    const char *path; // PATH
    const char *vela_terminal; // VELA_TERMINAL
    const char *config_home; // XDG_CONFIG_HOME, else $HOME/.config
};

// The process's environment (pointers into it).
void vela_terminal_env_get(struct vela_terminal_env *env);

enum vela_terminal_source {
    VELA_TERMINAL_CHOSEN = 1, // step 1
    VELA_TERMINAL_VARIABLE, // step 2
    VELA_TERMINAL_XDG_TERMINAL_EXEC, // step 3
};

struct vela_terminal {
    enum vela_terminal_source source;
    char *id; // the preferred desktop ID (step 1), else NULL
    char **argv; // the terminal alone, NULL-terminated; argv[0] a full path
    // "-e" for VELA_TERMINAL, NULL for the XDG launcher.
    char *exec_arg;
};

// Free a successful lookup with vela_terminal_free.
bool vela_terminal_find(const struct vela_terminal_env *env, struct vela_terminal *terminal);
void vela_terminal_free(struct vela_terminal *terminal);

// The command line: the terminal alone for an empty `command` (NULL or
// {NULL}), else the terminal running `command` (its exec argument, then
// the command's words). A terminal that should stay open after the command
// is not asked for: the caller keeps it open itself. Free with
// vela_terminal_strv_free.
char **vela_terminal_argv(const struct vela_terminal *terminal, const char *const *command);
void vela_terminal_strv_free(char **strv);

// The file of step 1: $XDG_CONFIG_HOME/vela-xdg-terminals.list. Allocated;
// NULL without XDG_CONFIG_HOME or HOME.
char *vela_terminal_choice_path(const struct vela_terminal_env *env);

#ifdef __cplusplus
}
#endif

#endif
