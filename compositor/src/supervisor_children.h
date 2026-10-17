// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SUPERVISOR_CHILDREN_H
#define VELA_SUPERVISOR_CHILDREN_H

// Count the supervisor's living direct children; optionally signal each one.
// Avoid heap allocations in the shutdown path. Returns -1 on /proc errors,
// otherwise the number of matching children seen (which may exit meanwhile).
int vela_supervisor_children(int signal);

#endif
