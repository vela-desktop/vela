// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SUPERVISOR_CHILD_H
#define VELA_SUPERVISOR_CHILD_H

#include <stdbool.h>
#include <stddef.h>

// How the supervisor starts each compositor (supervisor.c).

// The compositor's argv: `self`, the supervisor's arguments without
// --supervise, then --lock-state-fd=<lock_state_fd>, written into the
// caller's `option` buffer. NULL-terminated; free() the array (not the
// strings). Built before the first fork: the restart child must not
// allocate. NULL when out of memory or `option` is too small.
char **vela_supervisor_arguments(int argc, char **argv, const char *self, int lock_state_fd, char *option,
    size_t option_size);

// In the restart child, before exec: the inherited descriptors lose CLOEXEC
// and the environment names them. Every step that can fail is checked; false
// at the first failure (the child then exits with 127, never starting a
// compositor with a partial environment). Whether to start locked is not
// passed here: the compositor reads the shared page and the marker itself.
bool vela_supervisor_prepare_child(int socket_fd, const char *display, int runtime_fd, int lock_state_fd,
    bool restarted);

#endif
