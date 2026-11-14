// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_CLIENT_POLICY_H
#define VELA_CLIENT_POLICY_H

// Which Wayland globals each client may see and bind (docs/security-model.md).
//
// Privileged protocols (security_policy.h: screen capture, clipboard
// control, synthetic input, output configuration, layer shell, session lock,
// foreign toplevels) are withheld from:
//   - clients connected through a wp_security_context_v1 socket (Flatpak
//     1.15+ and other sandboxes that use it);
//   - clients whose process runs in a Flatpak sandbox without one;
//   - Xwayland, which needs none of them: X11 apps capture through the portal.
// Unsandboxed native clients of the same user keep them: Wayland cannot tell
// them apart, and the user's own tools (screenshots, the screen-sharing
// portal, clipboard managers) rely on them.
//
// The policy owns the display's global filter and also applies Xwayland's
// xdg-output and shell rules (x11_root.h).

#include <stdbool.h>

struct vela_server;
struct wl_client;
struct wl_global;

// Creates the security-context global and installs the display's filter.
// Freed with the display.
void vela_client_policy_init(struct vela_server *server);

// Whether `client` is sandboxed (security context or Flatpak). Cached per
// client.
bool vela_client_sandboxed(struct vela_server *server, const struct wl_client *client);

#endif
