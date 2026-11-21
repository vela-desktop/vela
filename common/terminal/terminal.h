// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The terminal (docs/keyboard.md §11): one lookup for Win+Enter
// (vela-terminal), the updater and Settings' Default apps > Terminal.
//
// 1. the user's choice: the first entry of
//    $XDG_CONFIG_HOME/vela-xdg-terminals.list (Settings writes it);
// 2. VELA_TERMINAL, a command with options;
// 3. xdg-terminal-exec, when installed;
// 4. the installed terminals: desktop files with
//    Categories=TerminalEmulator, in desktop-ID order;
// 5. a fixed list, when there are no desktop files.
//
// C17, no dependency but common/alloc. Nothing here is fast enough for a
// compositor's event loop: it reads directories.
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
    const char *config_dirs; // XDG_CONFIG_DIRS, else /etc/xdg
    const char *data_home; // XDG_DATA_HOME, else $HOME/.local/share
    const char *data_dirs; // XDG_DATA_DIRS, else /usr/local/share:/usr/share
    const char *current_desktop; // XDG_CURRENT_DESKTOP, for OnlyShowIn and NotShowIn
};

// The process's environment (pointers into it).
void vela_terminal_env_get(struct vela_terminal_env *env);

enum vela_terminal_source {
    VELA_TERMINAL_CHOSEN = 1, // step 1
    VELA_TERMINAL_VARIABLE, // step 2
    VELA_TERMINAL_XDG_TERMINAL_EXEC, // step 3
    VELA_TERMINAL_INSTALLED, // step 4
    VELA_TERMINAL_FIXED, // step 5
};

struct vela_terminal {
    enum vela_terminal_source source;
    char *id; // the desktop ID (steps 1 and 4), else NULL
    char **argv; // the terminal alone, NULL-terminated; argv[0] a full path
    // Put before a command: NULL or "" for nothing (xdg-terminal-exec,
    // "foot cmd"), "-e", "--"...
    char *exec_arg;
};

// The terminal, or false when there is none. `automatic` skips the user's
// choice (step 1): what Settings shows as "Automatic". Free with
// vela_terminal_free.
bool vela_terminal_find(const struct vela_terminal_env *env, bool automatic, struct vela_terminal *terminal);
void vela_terminal_free(struct vela_terminal *terminal);

// The command line: the terminal alone for an empty `command` (NULL or
// {NULL}), else the terminal running `command` (its exec argument, then
// the command's words). A terminal that should stay open after the command
// is not asked for: the caller keeps it open itself. Free with
// vela_terminal_strv_free.
char **vela_terminal_argv(const struct vela_terminal *terminal, const char *const *command);
void vela_terminal_strv_free(char **strv);

// The installed terminals of step 4, by desktop ID, sorted; NULL-terminated
// (maybe empty). Free with vela_terminal_strv_free.
char **vela_terminal_installed(const struct vela_terminal_env *env);

// The file of step 1: $XDG_CONFIG_HOME/vela-xdg-terminals.list. Allocated;
// NULL without XDG_CONFIG_HOME or HOME.
char *vela_terminal_choice_path(const struct vela_terminal_env *env);

#ifdef __cplusplus
}
#endif

#endif
