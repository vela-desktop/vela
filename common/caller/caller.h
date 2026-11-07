// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_CALLER_H
#define VELA_CALLER_H

// Who may call a Vela session daemon on D-Bus: vela-bluetooth
// (docs/bluetooth.md §7.3) and vela-update-daemon (docs/updates.md §7).
//
// This is not a wall against malicious code already running as the user;
// it keeps sandboxed apps, and for some calls any app but Vela's own UI
// (recognised by executable), from acting through the daemon.

#include <stdbool.h>
#include <sys/types.h>
#include <systemd/sd-bus.h>

enum vela_caller_trust {
    VELA_CALLER_SAME_USER,
    VELA_CALLER_UNSANDBOXED, // same user, not a Flatpak or a Snap
    VELA_CALLER_VELA_UI, // same user, not sandboxed, and one of Vela's UI executables
};

// The process is a Flatpak (/.flatpak-info in its root) or a Snap (a
// "snap." cgroup), as xdg-desktop-portal tells them. `proc` is /proc, or
// the tests' fake.
bool vela_caller_sandboxed(const char *proc, pid_t pid);

// `exe` (/proc/PID/exe's target) is one of the NULL-terminated `allowed`.
// " (deleted)" is accepted after it: a binary replaced by an update still
// came from that path, where only root installs.
bool vela_caller_exe_allowed(const char *exe, const char *const *allowed);

// The process `pidfd` refers to, from <proc>/self/fdinfo; -1 if it has
// exited or can't be read.
pid_t vela_caller_pidfd_pid(const char *proc, int pidfd);

// Checks the sender of `call` against `trust`. 0, or a negative errno with
// `error` set to `error_name` (and the refusal logged). `allowed` is the
// list of Vela's UI executables, for VELA_CALLER_VELA_UI.
int vela_caller_check(sd_bus_message *call, enum vela_caller_trust trust, const char *const *allowed,
    const char *error_name, sd_bus_error *error);

#endif
