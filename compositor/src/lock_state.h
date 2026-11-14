// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_LOCK_STATE_H
#define VELA_LOCK_STATE_H

#include <stdbool.h>

#include "path_security.h"

// The screen-lock state shared between the supervisor and every compositor it
// starts (docs/memory-allocation-policy.md, "The premise", option c).
//
// The supervisor creates one sealed, one-page memfd before its first fork and
// passes it to each compositor as --lock-state-fd=N. The compositor maps it,
// closes the descriptor (no application can inherit it) and stores "locked"
// before the lock becomes visible and "unlocked" only after a successful
// unlock. A store into an already mapped page cannot fail, so a crash at any
// moment, including one during the lock's fatal blackout, leaves a value the
// next compositor can trust. The value persists across restarts.
struct vela_lock_state;

#define VELA_LOCK_STATE_OPTION "lock-state-fd"

// Supervisor: create the sealed page, zero ("unlocked") and already backed by
// memory. Returns a CLOEXEC descriptor, or -1 with errno set.
int vela_lock_state_create(void);

// Compositor: parse the --lock-state-fd value. -1 when it is not a plain
// non-negative decimal descriptor number.
int vela_lock_state_parse_fd(const char *text);

// Compositor: validate that fd is a sealed one-page memfd and map it writable.
// On success the descriptor is closed and the page is returned; on failure
// NULL is returned and fd is left untouched. No heap allocation.
struct vela_lock_state *vela_lock_state_map(int fd);

// Unmap the page (tests; the compositor keeps it until it exits).
void vela_lock_state_unmap(struct vela_lock_state *state);

// Any value other than "unlocked" reads as locked. NULL reads as unlocked:
// without a page (no supervisor, or an older one) the marker decides alone.
bool vela_lock_state_locked(const struct vela_lock_state *state);

// No-op for NULL.
void vela_lock_state_store(struct vela_lock_state *state, bool locked);

// The restart decision with a mapped page: start locked unless the page says
// "unlocked" and the marker is positively absent.
bool vela_lock_start_locked(bool page_locked, enum vela_lock_marker marker);

// The whole decision at startup. `forced`: VELA_START_LOCKED from an older
// supervisor. Without a page (unsupervised, or an older supervisor) only
// `forced` counts, as before. *unreadable is set when the marker's state
// could not be established (which counts as locked).
bool vela_lock_restart_locked(const struct vela_lock_state *state, int runtime_fd, const char *display,
    bool forced, bool *unreadable);

#endif
