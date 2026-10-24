// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_LOCK_H
#define VELA_LOCK_H

// Screen lock and inactivity.
//
// The lock is ext-session-lock-v1: a program (vela-lock) asks to lock, and
// from then on the compositor shows only its surfaces, one per output, over
// black. Everything else is off: no windows, no shortcuts, no keyboard for
// apps. It unlocks only when the program says so (right password); if it
// crashes, the screen stays black and locked, and a new lock program can take
// its place (it's relaunched, at most 5 times a minute).
//
// Inactivity: after some minutes without input (10 by default; 0: never) the
// screen locks and, a few seconds later, turns off. Minutes and locking are in
// vela.conf ("screen-off", "lock-on-idle"; VELA_SCREEN_OFF and
// VELA_LOCK_ON_IDLE take precedence). An app can prevent it while showing
// something (a video: idle-inhibit). Apps learn the user is idle through
// ext-idle-notify. Caffeine (quick settings, "caffeine on|off|toggle", with an
// optional duration in minutes: "caffeine on 30") keeps the user "here" until
// turned off or the time runs out: neither of them, nor apps' idle
// notifications. Nested or headless nothing turns off, unless VELA_SCREEN_OFF
// is set (tests; minutes, fractions too).

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "lock_retry_policy.h"
#include <wayland-server-core.h>

struct vela_output;
struct vela_rect_node;
struct vela_server;
struct vela_tree;
struct wlr_idle_inhibit_manager_v1;
struct wlr_idle_notifier_v1;
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_v1;

#define VELA_LOCK_RESPAWNS 5 // at most, in one minute
#define VELA_CAFFEINE_MAX_MINUTES 1440.0 // 24 hours; fits the event-loop timer

struct vela_lock {
    struct vela_server *server;
    struct wlr_session_lock_manager_v1 *manager;
    struct wlr_session_lock_v1 *lock; // the active lock program, or NULL
    struct vela_tree *backdrop_tree; // at the bottom of the lock layer
    struct vela_rect_node **backdrop; // black, one per output
    int backdrop_count;
    int backdrop_capacity;
    // The outputs that still have to show black before telling the app
    // "locked".
    struct vela_output **waiting;
    int waiting_count;
    int waiting_capacity;
    pid_t launcher_pid; // child process group for a pending locker launch
    struct wl_event_source *respawn; // relaunches vela-lock if it goes away
    int64_t respawns[VELA_LOCK_RESPAWNS]; // when (ms), so as not to insist forever
    int respawn_count;
    struct vela_lock_retry retry; // registration timeout and bounded retry/cooldown
    bool fatal_exit_pending; // cannot safely continue this session
    struct wl_event_source *fatal_timer;
    struct vela_output **fatal_waiting;
    int fatal_waiting_count;
    int fatal_waiting_capacity;

    struct wlr_idle_notifier_v1 *idle_notifier;
    struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
    struct wl_event_source *idle_timer; // only in the real session
    int screen_off_ms; // 0: never
    bool lock_on_idle;
    bool locking; // locked for inactivity, outputs to turn off shortly
    bool screens_off;
    bool caffeine; // for this session only
    int64_t caffeine_until_ns; // CLOCK_MONOTONIC deadline, 0: no deadline
    int caffeine_duration_s; // originally chosen duration; 0 if indefinite or off
    struct wl_event_source *caffeine_timer; // turns itself off when time is up

    struct wl_listener new_lock;
    struct wl_listener new_surface;
    struct wl_listener unlock;
    struct wl_listener destroy;
    struct wl_listener new_inhibitor;
};

struct vela_lock *vela_lock_create(struct vela_server *server);
void vela_lock_destroy(struct vela_lock *lock);

// Win+L, inactivity, before suspend: starts vela-lock (VELA_LOCK picks
// another).
void vela_lock_screen(struct vela_lock *lock);
// Hides everything but the lock layer (black until vela-lock shows up) and
// tells the supervisor.
void vela_lock_engage(struct vela_lock *lock);
// The user is here: turns the outputs back on and restarts the wait.
void vela_lock_note_activity(struct vela_lock *lock);
// Caffeine: no inactivity (no lock, outputs on, apps never told idle).
void vela_lock_set_caffeine(struct vela_lock *lock, bool on);
// Caffeine for a while: accept only positive finite minutes and only if its
// expiry timer was armed successfully. On timer failure, fail closed (off).
bool vela_lock_set_caffeine_for(struct vela_lock *lock, double minutes);
// Seconds left (rounded up): 0 when off, -1 when on with no deadline.
int vela_lock_caffeine_remaining_s(struct vela_lock *lock);
// Rereads vela.conf (the "reload-config" command).
void vela_lock_load_settings(struct vela_lock *lock);
// On every delivered frame: the lock waits for black on every output.
void vela_lock_output_rendered(struct vela_lock *lock, struct vela_output *output);
// The outputs changed while locked.
void vela_lock_update_layout(struct vela_lock *lock);

#endif
