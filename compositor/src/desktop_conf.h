// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_DESKTOP_CONF_H
#define VELA_DESKTOP_CONF_H

#include <stdbool.h>

// desktop.conf: desktop=<name> for each virtual desktop in order (empty:
// "Desktop N"), sticky-app=<app_id> for each app shown on every desktop
// (app-ovunque= is the old name).
struct vela_desktop_conf_reader {
    void (*desktop)(void *data, const char *name);
    void (*sticky_app)(void *data, const char *app_id);
    void *data;
};

// Calls the reader for each entry of the file at `path`. A missing file is
// an empty one (true). false when it exists but could not be opened or read
// to the end: the caller must never rewrite it from what it got.
bool vela_desktop_conf_read(const char *path, const struct vela_desktop_conf_reader *reader);

#endif
