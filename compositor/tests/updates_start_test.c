// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela_updates_start (updates.c): VELA_UPDATE_DAEMON's command or "0", and
// without it the installed daemon, only in the real session.
#include "server.h"
#include "updates.h"

#undef NDEBUG // CI uses RelWithDebInfo: assertions must remain active.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool installed = true;
static int starts;
static char started[256];

bool vela_find_program(const char *name, const char *build_dir, char *command, size_t size)
{
    assert(strcmp(build_dir, "updates") == 0);
    snprintf(command, size, "/usr/lib/vela/%s", name);
    return installed;
}

bool vela_child_start(struct vela_child *child, struct wl_event_loop *loop, const char *command)
{
    ++starts;
    snprintf(started, sizeof(started), "%s", command);
    return true;
}

static void start(struct vela_server *server, const char *expected)
{
    starts = 0;
    started[0] = '\0';
    vela_updates_start(server);
    assert(starts == (expected ? 1 : 0));
    assert(!expected || strcmp(started, expected) == 0);
}

int main(void)
{
    static struct vela_server server;

    // Nested or headless: none, unless asked for.
    unsetenv("VELA_UPDATE_DAEMON");
    start(&server, NULL);
    setenv("VELA_UPDATE_DAEMON", "/tmp/daemon --verbose", 1);
    start(&server, "/tmp/daemon --verbose");

    // The real session: the installed one, if there is one.
    server.session = (struct wlr_session *)&server;
    setenv("VELA_UPDATE_DAEMON", "", 1);
    start(&server, "/usr/lib/vela/vela-update-daemon");
    installed = false;
    start(&server, NULL);
    setenv("VELA_UPDATE_DAEMON", "0", 1);
    installed = true;
    start(&server, NULL);
    return 0;
}
