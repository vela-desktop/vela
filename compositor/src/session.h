// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SESSION_H
#define VELA_SESSION_H

// The real session (from SDDM or a console): Vela is the desktop and tells the
// apps, and notifies systemd and D-Bus when it starts and ends
// (session/vela-session-env). Nested or headless, Vela stays a guest of the
// environment it finds.

#include <stdbool.h>

// A supervised compositor leaves systemd session teardown to its supervisor
// when restarting after a terminal renderer failure. Direct sessions cannot
// delegate that final stop to anyone else.
static inline bool vela_session_should_stop(bool has_session, bool supervised, bool terminal_failure)
{
    return has_session && (!supervised || !terminal_failure);
}

// Only the values nobody (SDDM, the user) has chosen already.
void vela_session_set_environment(void);
// "start" or "stop": waits for vela-session-env to finish.
void vela_session_run_hook(const char *action);

#endif
