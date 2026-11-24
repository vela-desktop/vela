// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shell.h"

#include "alloc/alloc.h"
#include "focus.h"
#include "output.h"
#include "process.h"
#include "server.h"
#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/pidfd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_output.h>

bool vela_shell_send(struct vela_server *server, const char *line)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !server->socket_name[0]) {
        return false;
    }
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (!vela_format(address.sun_path, sizeof(address.sun_path), "%s/vela-shell-%s.sock", runtime_dir,
            server->socket_name)) {
        return false;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return false;
    }
    bool sent = false;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        // One newline-terminated frame: even if stream reads split or interleave,
        // the shell cannot dispatch the command with another frame's output.
        struct vela_output *output = vela_active_output(server);
        const char *name = output ? output->wlr->name : "";
        const char prefix[] = "active-output ";
        size_t name_length = strlen(name);
        size_t line_length = strlen(line);
        size_t offset = vela_size_add(sizeof(prefix), name_length);
        size_t length = vela_size_add(offset, vela_size_add(line_length, 1));
        char *message = VELA_NEW_N(char, length);
        memcpy(message, prefix, sizeof(prefix) - 1);
        memcpy(message + sizeof(prefix) - 1, name, name_length);
        message[offset - 1] = '\t';
        memcpy(message + offset, line, line_length);
        message[length - 1] = '\n';
        sent = send(fd, message, length, MSG_NOSIGNAL) == (ssize_t)length;
        free(message);
    } else {
        wlr_log(WLR_DEBUG, "Shell not reachable on %s", address.sun_path);
    }
    close(fd);
    return sent;
}

// -------------------------------------------------------------- supervision --

void vela_shell_start(struct vela_server *server, const char *command)
{
    vela_child_start(&server->shell, server->loop, command);
}

void vela_shell_stop(struct vela_server *server)
{
    vela_child_stop(&server->shell);
}
