// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "polkit.h"

#include "config.h"
#include "process.h"
#include "server.h"
#include "util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/util/log.h>

void vela_polkit_agent_start(struct vela_server *server)
{
    char command[PATH_MAX + 2] = "";
    const char *custom = getenv("VELA_POLKIT_AGENT");
    if (custom && *custom) {
        if (strcmp(custom, "0") == 0) {
            return;
        }
        snprintf(command, sizeof(command), "%s", custom);
    } else {
        if (!server->session) {
            return;
        }
        struct vela_config settings;
        vela_config_read(&settings);
        bool enabled = vela_config_flag(&settings, "polkit-agent", true);
        vela_config_finish(&settings);
        if (!enabled) {
            wlr_log(WLR_INFO, "Polkit agent disabled in vela.conf");
            return;
        }
        if (!vela_find_program("vela-polkit-agent", "polkit", command, sizeof(command))) {
            wlr_log(WLR_ERROR, "Polkit agent: vela-polkit-agent not found");
            return;
        }
    }
    wlr_log(WLR_INFO, "Starting the polkit agent: %s", command);
    vela_child_start(&server->polkit_agent, server->loop, command);
}
