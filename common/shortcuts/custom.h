// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_CUSTOM_SHORTCUTS_H
#define VELA_CUSTOM_SHORTCUTS_H

// The user's own shortcuts (docs/keyboard.md §9.4):
// $XDG_CONFIG_HOME/vela/shortcuts.conf, written by Settings, one per line:
//
//   Ctrl+Alt+t=app org.kde.konsole.desktop
//   Shift+Super+k=command kcalc
//   Super+FK01=open https://docs.example.org/
//
// The compositor reads only the combinations; the shell, asked to run one
// ("shortcut <combination>"), reads the line again and runs its target.

#include "shortcuts.h"

#include <stdbool.h>
#include <stddef.h>

// A line's limit, its newline left out.
#define VELA_CUSTOM_LINE_MAX 4096
// More are ignored.
#define VELA_CUSTOM_MAX 256

enum vela_custom_kind {
    VELA_CUSTOM_APP, // a desktop file ID
    VELA_CUSTOM_COMMAND, // a shell command line
    VELA_CUSTOM_OPEN, // a URL or an absolute path
};

struct vela_custom_line {
    struct vela_combo combo;
    enum vela_custom_kind kind;
    const char *target; // inside the parsed line
    size_t target_length;
};

// One line of the file, without its newline: false for a comment, an empty
// line, or one that doesn't parse (longer than VELA_CUSTOM_LINE_MAX, a
// control character, a bad combination, an unknown kind, an empty or
// invalid target).
bool vela_custom_line_parse(const char *line, size_t length, struct vela_custom_line *out);

// Whether `target` is fit for a line of `kind` (no control character, the
// line's limit, an app's ".desktop", an absolute path or a URL to open).
bool vela_custom_target_valid(enum vela_custom_kind kind, const char *target, size_t length);

const char *vela_custom_kind_name(enum vela_custom_kind kind);

// $XDG_CONFIG_HOME/vela/shortcuts.conf, or ~/.config/vela/shortcuts.conf.
bool vela_custom_path(char *out, size_t size);

#endif
