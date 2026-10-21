// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_AUTOSCROLL_H
#define VELA_AUTOSCROLL_H

// Middle-click autoscroll, like Windows: the middle button pressed on a
// window starts it (the click doesn't reach the app: no paste, no new tab);
// the cursor shows the theme's "all-scroll" and moving it away from where
// it was pressed scrolls that way, faster the farther it goes. A short click
// leaves it on until any button or Esc; held and dragged, it ends on the
// release. It also ends when the window loses the keyboard or goes away, on
// lock, Alt+Tab, Task View (any shell piece taking the keyboard), a pointer
// constraint (games) or the output going away.
//
// The scrolling is continuous: every output frame (on the vblank grid of the
// frame clock, frame_clock.h) the speed times the frame time, in fractions of
// a pixel, as wl_pointer axis events with source "continuous" (the one
// libinput uses for scrolling with a held trackpoint button, the same
// gesture), never discrete steps. Meanwhile the app gets no motion (hover
// effects stay still) and no cursor changes; on the way out it gets
// axis_stop, then leave and enter again, so it sets its cursor back.
//
// vela.conf: middle-click-autoscroll=yes (the default; set to no to
// restore normal middle-click behavior), autoscroll-speed=5 (1-10);
// (terminals are left out unless a rule says yes). Two more, both yes:
// autoscroll-click (a click turns it on) and autoscroll-hold (held and
// dragged, it scrolls until the release). With autoscroll-click=no the
// middle press waits (4 px or 200 ms) before it is a held autoscroll;
// released earlier, press and release reach the app together (paste, new
// tab). A timer runs only while it is on or waiting.

#include <stdbool.h>
#include <stdint.h>

struct vela_server;
struct wlr_pointer_button_event;

// Below this distance from the starting point (per axis, logical px) it
// doesn't scroll; beyond it, the speed in px/s for that axis.
#define VELA_AUTOSCROLL_DEAD_ZONE 12.0
double vela_autoscroll_speed(double offset, int speed_setting);

struct vela_autoscroll *vela_autoscroll_create(struct vela_server *server);
void vela_autoscroll_destroy(struct vela_autoscroll *autoscroll);
// Rereads vela.conf (startup and "reload-config"); turned off, it stops.
void vela_autoscroll_load_settings(struct vela_autoscroll *autoscroll);

bool vela_autoscroll_active(const struct vela_autoscroll *autoscroll);
// Before the button reaches anyone: true if autoscroll took it (started,
// ended, or the release of a press it took).
bool vela_autoscroll_button(struct vela_autoscroll *autoscroll, const struct wlr_pointer_button_event *event);
// The cursor moved: true if it is on (the motion goes to nobody); a held-back
// middle press that moves far enough starts it here.
bool vela_autoscroll_motion(struct vela_autoscroll *autoscroll);
// A key: Esc ends it and is taken (true).
bool vela_autoscroll_key(struct vela_autoscroll *autoscroll, const uint32_t *syms, int count, bool pressed);

#endif
