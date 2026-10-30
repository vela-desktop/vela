// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "child.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

extern char **environ;

pid_t vela_up_spawn(char *const *argv, int *out, bool detach)
{
    int pipe_fds[2] = { -1, -1 };
    if (out && pipe2(pipe_fds, O_CLOEXEC) < 0) {
        return -errno;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (out) {
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    }
    // The daemon blocks the signals its loop handles; a child must not
    // inherit that (vela-update stops on TERM and HUP).
    sigset_t none;
    sigemptyset(&none);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (detach) {
        flags |= POSIX_SPAWN_SETSID;
    }
    posix_spawnattr_setflags(&attributes, flags);
    pid_t pid;
    int r = posix_spawnp(&pid, argv[0], &actions, &attributes, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (out) {
        close(pipe_fds[1]);
        if (r != 0) {
            close(pipe_fds[0]);
        } else {
            *out = pipe_fds[0];
        }
    }
    return r != 0 ? -r : pid;
}
