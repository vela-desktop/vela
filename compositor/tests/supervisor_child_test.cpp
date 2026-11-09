// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

extern "C" {
#include "supervisor_child.h"
}

#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

TEST(SupervisorChild, ArgumentsDropSuperviseAndAddTheLockState)
{
    char supervise[] = "--supervise", shell[] = "-s", command[] = "vela-shell", self[] = "/usr/bin/vela-compositor";
    char *argv[] = { self, supervise, shell, command, nullptr };
    char option[32];
    char **arguments = vela_supervisor_arguments(4, argv, "/proc/self/exe-target", 7, option, sizeof(option));
    ASSERT_NE(arguments, nullptr);
    EXPECT_STREQ(arguments[0], "/proc/self/exe-target");
    EXPECT_STREQ(arguments[1], "-s");
    EXPECT_STREQ(arguments[2], "vela-shell");
    EXPECT_STREQ(arguments[3], "--lock-state-fd=7");
    EXPECT_EQ(arguments[4], nullptr);
    free(arguments);

    // A buffer too small for the option is refused, never truncated.
    char tiny[8];
    EXPECT_EQ(vela_supervisor_arguments(4, argv, "x", 7, tiny, sizeof(tiny)), nullptr);
    EXPECT_EQ(vela_supervisor_arguments(0, argv, "x", 7, option, sizeof(option)), nullptr);
}

// Runs the child preparation in a forked child, as the supervisor does, and
// reports what the exec'd compositor would see.
static int prepare_in_child(int socket_fd, int runtime_fd, int lock_state_fd, bool restarted)
{
    pid_t pid = fork();
    if (pid == 0) {
        setenv("VELA_START_LOCKED", "1", 1); // stray: must be removed
        if (!vela_supervisor_prepare_child(socket_fd, "wayland-7", runtime_fd, lock_state_fd, restarted)) {
            _exit(10);
        }
        char expected[16];
        snprintf(expected, sizeof(expected), "%d", socket_fd);
        const char *socket_text = getenv("VELA_WAYLAND_SOCKET_FD");
        const char *display = getenv("VELA_WAYLAND_DISPLAY");
        bool ok = socket_text && strcmp(socket_text, expected) == 0 && display && strcmp(display, "wayland-7") == 0;
        snprintf(expected, sizeof(expected), "%d", runtime_fd);
        const char *runtime_text = getenv("VELA_RUNTIME_DIR_FD");
        ok = ok && runtime_text && strcmp(runtime_text, expected) == 0;
        ok = ok && !getenv("VELA_START_LOCKED");
        ok = ok && (getenv("VELA_RESTARTED") != nullptr) == restarted;
        for (int fd : { socket_fd, runtime_fd, lock_state_fd }) {
            ok = ok && (fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0; // inherited by exec
        }
        _exit(ok ? 0 : 11);
    }
    int status = 0;
    EXPECT_EQ(waitpid(pid, &status, 0), pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

TEST(SupervisorChild, PreparesDescriptorsAndEnvironment)
{
    unsetenv("VELA_RESTARTED");
    int fds[3];
    for (int &fd : fds) {
        fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        ASSERT_GE(fd, 0);
    }
    EXPECT_EQ(prepare_in_child(fds[0], fds[1], fds[2], false), 0);
    EXPECT_EQ(prepare_in_child(fds[0], fds[1], fds[2], true), 0);
    // A descriptor that cannot be handed over fails the whole preparation.
    EXPECT_EQ(prepare_in_child(fds[0], fds[1], -1, false), 10);
    EXPECT_EQ(prepare_in_child(-1, fds[1], fds[2], false), 10);
    for (int fd : fds) {
        close(fd);
    }
}
