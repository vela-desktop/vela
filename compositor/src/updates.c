// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "updates.h"

#include "process.h"
#include "server.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

void vela_updates_start(struct vela_server *server)
{
    char command[PATH_MAX + 2] = "";
    const char *custom = getenv("VELA_UPDATE_DAEMON");
    if (custom && *custom) {
        if (strcmp(custom, "0") == 0) {
            return;
        }
        snprintf(command, sizeof(command), "%s", custom);
    } else {
        if (!server->session) {
            return;
        }
        if (!vela_find_program("vela-update-daemon", "updates", command, sizeof(command))) {
            wlr_log(WLR_ERROR, "Updates: vela-update-daemon not found");
            return;
        }
    }
    wlr_log(WLR_INFO, "Starting updates: %s", command);
    vela_child_start(&server->updates, server->loop, command);
}
