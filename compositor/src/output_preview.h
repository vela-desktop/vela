// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef VELA_OUTPUT_PREVIEW_H
#define VELA_OUTPUT_PREVIEW_H

#include <stdbool.h>
struct vela_server;
struct wlr_output_configuration_v1;

void vela_output_preview_init(struct vela_server *server);
void vela_output_preview_finish(struct vela_server *server);
// Reject competing, expired or abandoned protected configurations.
bool vela_output_preview_can_apply(struct vela_server *server, struct wlr_output_configuration_v1 *config);
// Takes ownership of the snapshot only when this is a protected apply.
bool vela_output_preview_applied(struct vela_server *server, struct wlr_output_configuration_v1 *config,
    struct wlr_output_configuration_v1 *before, bool succeeded, bool restored);

#endif
