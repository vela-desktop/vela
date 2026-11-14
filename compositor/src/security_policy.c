// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "security_policy.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Keep in sync with docs/security-model.md, "Privileged protocols".
static const char *const privileged[] = {
    // Reading the screen or other windows.
    "zwlr_screencopy_manager_v1",
    "ext_image_copy_capture_manager_v1",
    "ext_output_image_capture_source_manager_v1",
    "ext_foreign_toplevel_image_capture_source_manager_v1",
    // Listing and driving other clients' windows.
    "ext_foreign_toplevel_list_v1",
    "zwlr_foreign_toplevel_manager_v1",
    // The clipboard without focus.
    "zwlr_data_control_manager_v1",
    "ext_data_control_manager_v1",
    // Synthetic input.
    "zwlr_virtual_pointer_manager_v1",
    "zwp_virtual_keyboard_manager_v1",
    // Outputs and the desktop itself.
    "zwlr_output_manager_v1",
    "vela_output_preview_manager_v1",
    "zwlr_layer_shell_v1",
    "ext_session_lock_manager_v1",
    // When the user is away.
    "ext_idle_notifier_v1",
    // A sandboxed client must not mint new security contexts.
    "wp_security_context_manager_v1",
};

bool vela_global_privileged(const char *interface)
{
    if (!interface) {
        return true; // unknown: withhold
    }
    for (size_t i = 0; i < sizeof(privileged) / sizeof(privileged[0]); ++i) {
        if (strcmp(interface, privileged[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool word(const char *line, const char *command)
{
    size_t length = strlen(command);
    return strncmp(line, command, length) == 0 && (line[length] == '\0' || line[length] == ' ');
}

bool vela_command_test_only(const char *line)
{
    return line && (word(line, "test-output") || word(line, "test-power") || word(line, "test-shell"));
}

bool vela_pid_in_flatpak(pid_t pid)
{
    if (pid <= 0) {
        return false;
    }
    char path[64];
    int written = snprintf(path, sizeof(path), "/proc/%ld/root/.flatpak-info", (long)pid);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return false;
    }
    // Opened, not stat()ed: never follow a symlink planted in the sandbox.
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
}

bool vela_shm_layout_valid(int width, int height, size_t stride, uint32_t bpp)
{
    if (width <= 0 || height <= 0 || bpp == 0 || bpp > 16) {
        return false;
    }
    size_t row = (size_t)width * bpp;
    if (row / bpp != (size_t)width || stride < row) {
        return false;
    }
    // The last byte read: (height - 1) * stride + row - 1.
    size_t rows = (size_t)height - 1;
    return rows == 0 || stride <= (SIZE_MAX - row) / rows;
}

bool vela_dmabuf_layout_valid(int width, int height, int n_planes, const uint32_t *offset, const uint32_t *stride,
    uint32_t bpp, uint64_t size)
{
    if (width <= 0 || height <= 0 || n_planes < 1 || n_planes > 4 || !offset || !stride || bpp == 0) {
        return false;
    }
    // Linear rows of plane 0 can't be shorter than their pixels. Tiled
    // modifiers have strides in tiles, never below this either.
    if ((uint64_t)stride[0] < (uint64_t)width * bpp) {
        return false;
    }
    for (int i = 0; i < n_planes; ++i) {
        if (stride[i] == 0) {
            return false;
        }
    }
    if (size == 0) {
        return true; // the exporter can't say: the driver checks the rest
    }
    // 32-bit operands in 64-bit arithmetic: no overflow.
    if ((uint64_t)offset[0] + (uint64_t)stride[0] * (uint64_t)height > size) {
        return false;
    }
    for (int i = 1; i < n_planes; ++i) {
        if ((uint64_t)offset[i] >= size || (uint64_t)offset[i] + stride[i] > size) {
            return false;
        }
    }
    return true;
}
