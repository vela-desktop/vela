// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BLUETOOTH_H
#define VELA_BLUETOOTH_H

// vela-bluetooth (docs/bluetooth.md §6.1): the compositor starts it as a
// supervised child, so it is in the compositor's logind session. Lives in
// vela_server.bluetooth.
//
// VELA_BLUETOOTH names another command, or "0" for none. Without it, the
// daemon starts only in the real (DRM) session: nested or headless, it must
// not register an agent for the user's real Bluetooth.

struct vela_server;

void vela_bluetooth_start(struct vela_server *server);

#endif
