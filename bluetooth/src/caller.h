// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BT_CALLER_H
#define VELA_BT_CALLER_H

// Who may call org.vela.Bluetooth (docs/bluetooth.md §7.3).
//
// Any unsandboxed process of the user may already pair, connect and remove
// devices through BlueZ with its own agent: those calls need only the same
// user, not sandboxed. What Vela adds is its agent's answers, reserved to
// Vela's own UI, recognised by executable. This is not a wall against
// malicious code already running as the user; it keeps other apps from
// answering through Vela.

#include <stdbool.h>
#include <sys/types.h>
#include <systemd/sd-bus.h>

enum vela_bt_trust {
    VELA_BT_SAME_USER,
    VELA_BT_UNSANDBOXED, // same user, not a Flatpak or a Snap
    VELA_BT_VELA_UI, // same user, not sandboxed, and one of Vela's UI executables
};

// The process is a Flatpak (/.flatpak-info in its root) or a Snap (a
// "snap." cgroup), as xdg-desktop-portal tells them. `proc` is /proc, or
// the tests' fake.
bool vela_bt_sandboxed(const char *proc, pid_t pid);

// `exe` (/proc/PID/exe's target) is one of the NULL-terminated `allowed`.
// " (deleted)" is accepted after it: a binary replaced by an update still
// came from that path, where only root installs.
bool vela_bt_exe_allowed(const char *exe, const char *const *allowed);

// The process `pidfd` refers to, from <proc>/self/fdinfo; -1 if it has
// exited or can't be read.
pid_t vela_bt_pidfd_pid(const char *proc, int pidfd);

// Checks the sender of `call` against `trust`. 0, or a negative errno with
// `error` set to org.vela.Bluetooth.Error.NotAllowed (and the refusal
// logged). `allowed` is the list of Vela's UI executables.
int vela_bt_check_caller(sd_bus_message *call, enum vela_bt_trust trust, const char *const *allowed,
    sd_bus_error *error);

#endif
