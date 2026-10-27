// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bluetooth.h"

#include "process.h"
#include "server.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

void vela_bluetooth_start(struct vela_server *server)
{
    char command[PATH_MAX + 2] = "";
    const char *custom = getenv("VELA_BLUETOOTH");
    if (custom && *custom) {
        if (strcmp(custom, "0") == 0) {
            return;
        }
        snprintf(command, sizeof(command), "%s", custom);
    } else {
        if (!server->session) {
            return;
        }
        if (!vela_find_program("vela-bluetooth", "bluetooth", command, sizeof(command))) {
            wlr_log(WLR_ERROR, "Bluetooth: vela-bluetooth not found");
            return;
        }
    }
    wlr_log(WLR_INFO, "Starting Bluetooth: %s", command);
    vela_child_start(&server->bluetooth, server->loop, command);
}
