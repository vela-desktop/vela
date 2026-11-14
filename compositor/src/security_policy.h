// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SECURITY_POLICY_H
#define VELA_SECURITY_POLICY_H

// The compositor's trust decisions that need neither wlroots nor a GPU, so
// that the tests can cover them (docs/security-model.md).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Wayland globals that read the screen, the clipboard or other windows,
// inject input, reconfigure outputs or take over the screen: withheld from
// sandboxed clients and from Xwayland. `interface` is the global's interface
// name, as wl_global_get_interface() reports it.
bool vela_global_privileged(const char *interface);

// Command-socket lines that exist only for the functional tests
// (test-output, test-power, test-shell): refused unless the compositor was
// started for tests (VELA_TEST_COMMANDS=1 and the headless backend).
bool vela_command_test_only(const char *line);

// Whether a process runs inside a Flatpak sandbox, from its /proc entry.
// For a sandbox that does not use wp_security_context_v1 (older Flatpak).
// False when it cannot be established (process gone, /proc not readable).
bool vela_pid_in_flatpak(pid_t pid);

// A shared-memory buffer: `stride` bytes per row, `bpp` bytes per pixel. The
// rows must hold their pixels and the last byte read must be addressable.
bool vela_shm_layout_valid(int width, int height, size_t stride, uint32_t bpp);

// A single-buffer dmabuf (every plane in the same dma-buf). `size` is the
// dma-buf's size as lseek(SEEK_END) reports it, 0 when the exporter does not
// say: then only what can be checked without it is. Plane 0 holds the pixels
// (`bpp` bytes each); further planes are auxiliary (compression metadata) and
// must start inside the buffer.
bool vela_dmabuf_layout_valid(int width, int height, int n_planes, const uint32_t *offset, const uint32_t *stride,
    uint32_t bpp, uint64_t size);

#endif
