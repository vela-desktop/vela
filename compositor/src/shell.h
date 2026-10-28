// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_SHELL_H
#define VELA_SHELL_H

// The Qt shell: the startup command (the shell) that the compositor launches
// and relaunches when it fails, and the messages it sends it.

struct vela_server;

#include <stdbool.h>

// Launches `command` and relaunches it when it fails (not when it exits
// cleanly, nor when it keeps failing right after starting). The shell lives
// in vela_server.shell (a vela_child, process.h).
void vela_shell_start(struct vela_server *server, const char *command);
// We are shutting down: the shell going away must not be relaunched.
void vela_shell_stop(struct vela_server *server);

// One line of text on the shell's Unix socket
// ($XDG_RUNTIME_DIR/vela-shell-<WAYLAND_DISPLAY>.sock), such as "toggle-start"
// or "accessibility {...}". Never blocks: without a shell the message is
// simply lost. Returns true only when the message reached the socket.
bool vela_shell_send(struct vela_server *server, const char *line);

#endif
