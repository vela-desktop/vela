// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_UPDATES_H
#define VELA_UPDATES_H

// vela-update-daemon (docs/updates.md §6): the compositor starts it as a
// supervised child, so it lives as long as the session and finds the
// compositor among its parents. Lives in vela_server.updates.
//
// VELA_UPDATE_DAEMON names another command, or "0" for none. Without it, the
// daemon starts only in the real (DRM) session: nested or headless, it
// must not check for updates in the user's name.

struct vela_server;

void vela_updates_start(struct vela_server *server);

#endif
