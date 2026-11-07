// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_ERROR_SCREEN_H
#define VELA_ERROR_SCREEN_H

// Unsupported GPUs cannot recover by restarting the compositor.
#define VELA_EXIT_UNSUPPORTED_GPU 78
// A failed secure lock marker must end the session without compositor restart.
#define VELA_EXIT_LOCK_UNSAFE 77
#define VELA_EXIT_DEVICE_LOST 79 // supervisor may restart within crash budget

// Best-effort error message using DRM dumb buffers, no Vulkan or Wayland.
void vela_error_screen_no_vulkan(int drm_fd);

#endif
