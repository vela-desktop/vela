// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SUPERVISOR_H
#define VELA_SUPERVISOR_H

#include <stdbool.h>
#include <stddef.h>

// vela-compositor --supervise ...: holds the Wayland socket and restarts the
// compositor if it crashes. Returns the process exit code.
int vela_supervise(int argc, char **argv);

// vela-session-env (ties the session to systemd): next to the executable
// (build directory) or installed. false if there is none.
bool vela_session_hook_path(char *out, size_t size);

#endif
