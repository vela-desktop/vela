// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_UP_CHILD_H
#define VELA_UP_CHILD_H

#include <stdbool.h>
#include <sys/types.h>

// Starts argv[0] (looked up in PATH) with stdin from /dev/null, no blocked
// signals, and stdout to a pipe whose read end goes to `out` when it isn't
// NULL. `detach`: in a session of its own, like an app the user opened.
// The PID, or a negative errno.
pid_t vela_up_spawn(char *const *argv, int *out, bool detach);

#endif
