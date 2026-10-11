// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_PATH_SECURITY_H
#define VELA_PATH_SECURITY_H

#include <stdbool.h>
#include <stddef.h>

// Single path component: no separators, "."/"..", or control characters.
bool vela_path_component_safe(const char *name);

// Relative path (icon theme subdirectories may contain '/'): components
// must not be empty, "." or ".."; absolute paths are rejected.
bool vela_relative_path_safe(const char *path);

// Open the actual XDG_RUNTIME_DIR (absolute, private, owned), rejecting a
// final symlink. Caller owns the returned O_PATH directory fd.
int vela_runtime_dir_open(const char *path);

// XDG_RUNTIME_DIR must be an absolute private directory owned by this UID.
// Symlink runtime directories are not accepted.
bool vela_runtime_dir_private(const char *path);

// Validate an already-open runtime directory descriptor.
bool vela_runtime_dir_fd_private(int fd);

// Create/check/remove the screen-lock marker relative to the exact runtime
// directory inode opened by the supervisor. The display is one path component.
bool vela_lock_marker_create_at(int runtime_fd, const char *display);
bool vela_lock_marker_present_at(int runtime_fd, const char *display);
bool vela_lock_marker_remove_at(int runtime_fd, const char *display);

// The lock marker stays directly within the private runtime directory.
// This builder is also tested without having to start a compositor.
bool vela_lock_flag_path_for_runtime(const char *runtime_dir, const char *display,
    char *out, size_t size);

#endif
