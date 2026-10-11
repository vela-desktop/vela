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

// XDG_RUNTIME_DIR must be an absolute 0700-style directory owned by this UID.
// Symlink runtime directories are not accepted.
bool vela_runtime_dir_private(const char *path);

// The lock marker stays directly within the private runtime directory.
// This builder is also tested without having to start a compositor.
bool vela_lock_flag_path_for_runtime(const char *runtime_dir, const char *display,
    char *out, size_t size);

#endif
