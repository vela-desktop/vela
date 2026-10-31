// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_UP_CHILD_H
#define VELA_UP_CHILD_H

#include <stdbool.h>
#include <sys/types.h>

enum vela_up_spawn_mode {
    // In a process group of its own, led by the child (its PID is the
    // group's ID): kill(-pid, ...) stops it with everything it started.
    VELA_UP_SPAWN_GROUP,
    // In a session of its own, like an app the user opened.
    VELA_UP_SPAWN_DETACH,
};

// Starts argv[0] (looked up in PATH) with stdin from /dev/null, no blocked
// signals, and stdout to a pipe whose read end goes to `out` when it isn't
// NULL. The PID, or a negative errno.
pid_t vela_up_spawn(char *const *argv, int *out, enum vela_up_spawn_mode mode);

#endif
