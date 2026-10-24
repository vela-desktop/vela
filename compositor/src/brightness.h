// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_BRIGHTNESS_H
#define VELA_BRIGHTNESS_H

// The brightness of every output (docs/brightness.md §6, §7): the compositor
// owns the model (brightness_model.h) and starts one vela-brightness per
// output, which touches the hardware. Lives in vela_server.brightness.
//
// VELA_BRIGHTNESS names another helper command, or "0" for none. Without it,
// helpers start only in the real (DRM) session, for DRM outputs: nested or
// headless the monitors belong to someone else. VELA_BRIGHTNESS_FAKE=<file>
// (fake monitors, brightness/src/fake.h) starts them anywhere, for every
// output, without touching any hardware. brightness=no in vela.conf starts
// none.

#include <stdbool.h>

struct vela_buffer;
struct vela_output;
struct vela_server;

// After the backend started: the outputs it announced get their helpers.
void vela_brightness_start(struct vela_server *server);
// At shutdown: the helpers are told to go, nothing waits for them.
void vela_brightness_destroy(struct vela_server *server);
// vela.conf changed.
void vela_brightness_load_settings(struct vela_server *server);

void vela_brightness_output_added(struct vela_output *output);
void vela_brightness_output_removed(struct vela_output *output);

// The `brightness` answer, and what `watch brightness` receives:
// {"outputs":[...]}, one entry per output that has a helper (§7.2).
void vela_brightness_json(struct vela_server *server, struct vela_buffer *json);

// From the command socket. False when the output has no brightness to set
// now, or the value is not 0 to 1.
bool vela_brightness_set(struct vela_server *server, const char *output, double value);
void vela_brightness_probe(struct vela_server *server, const char *output);
// Whether any connection shows a slider (live sync, §5.5).
void vela_brightness_live(struct vela_server *server, bool on);

#endif
