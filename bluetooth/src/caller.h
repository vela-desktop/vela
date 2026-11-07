// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BT_CALLER_H
#define VELA_BT_CALLER_H

// Who may call org.vela.Bluetooth (docs/bluetooth.md §7.3).
//
// Any unsandboxed process of the user may already pair, connect and remove
// devices through BlueZ with its own agent: those calls need only the same
// user, not sandboxed. What Vela adds is its agent's answers, reserved to
// Vela's own UI, recognised by executable. The check itself is shared with
// the other session daemons (common/caller).

#include "caller/caller.h"

#define VELA_BT_SAME_USER VELA_CALLER_SAME_USER
#define VELA_BT_UNSANDBOXED VELA_CALLER_UNSANDBOXED
#define VELA_BT_VELA_UI VELA_CALLER_VELA_UI

// 0, or a negative errno with `error` set to
// org.vela.Bluetooth.Error.NotAllowed.
static inline int vela_bt_check_caller(sd_bus_message *call, enum vela_caller_trust trust,
    const char *const *allowed, sd_bus_error *error)
{
    return vela_caller_check(call, trust, allowed, "org.vela.Bluetooth.Error.NotAllowed", error);
}

#endif
