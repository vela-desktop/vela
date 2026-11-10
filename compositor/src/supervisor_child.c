// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "supervisor_child.h"

#include "alloc/alloc.h"
#include "lock_state.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char **vela_supervisor_arguments(int argc, char **argv, const char *self, int lock_state_fd, char *option,
    size_t option_size)
{
    int written = snprintf(option, option_size, "--%s=%d", VELA_LOCK_STATE_OPTION, lock_state_fd);
    if (written < 0 || (size_t)written >= option_size || argc < 1) {
        return NULL;
    }
    char **arguments = VELA_NEW_N(char *, (size_t)argc + 2);
    int count = 0;
    arguments[count++] = (char *)self;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--supervise") != 0) {
            arguments[count++] = argv[i];
        }
    }
    arguments[count++] = option;
    arguments[count] = NULL;
    return arguments;
}

bool vela_supervisor_prepare_child(int socket_fd, const char *display, int runtime_fd, int lock_state_fd,
    bool restarted)
{
    // The compositor restores CLOEXEC on the runtime directory and closes the
    // lock state before it launches any application.
    if (fcntl(socket_fd, F_SETFD, 0) != 0 || fcntl(runtime_fd, F_SETFD, 0) != 0
        || fcntl(lock_state_fd, F_SETFD, 0) != 0) {
        return false;
    }
    char fd[16];
    snprintf(fd, sizeof(fd), "%d", socket_fd);
    if (setenv("VELA_WAYLAND_SOCKET_FD", fd, 1) != 0 || setenv("VELA_WAYLAND_DISPLAY", display, 1) != 0) {
        return false;
    }
    snprintf(fd, sizeof(fd), "%d", runtime_fd);
    return setenv("VELA_RUNTIME_DIR_FD", fd, 1) == 0 && (!restarted || setenv("VELA_RESTARTED", "1", 1) == 0)
        && unsetenv("VELA_START_LOCKED") == 0;
}
