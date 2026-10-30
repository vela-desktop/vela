// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_PROCESS_H
#define VELA_PROCESS_H

// Programs launched by Vela: a shell command (/bin/sh -c), in a session of its
// own and with signals unblocked (wlroots blocks some to handle them in its
// loop).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct wl_event_loop;
struct wl_event_source;

// One of Vela's programs, as a quoted path for a shell command: next to the
// compositor, in the build directory (`build_dir`, a sibling of the
// compositor's), or installed in VELA_LIBEXECDIR.
bool vela_find_program(const char *name, const char *build_dir, char *command, size_t size);

// In the child process: runs `command` and never returns.
void vela_exec_shell(const char *command) __attribute__((noreturn));

// Launches `command` detached from Vela (double fork: init adopts it and no
// zombie is left).
void vela_spawn(const char *command);

// A child the compositor launches and relaunches when it fails: the shell
// and the polkit agent (docs/polkit-agent.md §7). A pidfd in the event loop
// tells when it ends. Not relaunched when it exits cleanly (0 or
// `final_exit_code`), nor when it keeps failing right after starting.
//
// With `exited` set, the owner decides instead: the child is never relaunched
// here, and `exited` gets its wait status (vela-brightness, whose socket is
// made anew for each process).
struct vela_child {
    const char *name; // for the log
    int death_signal; // sent to the child when the compositor dies
    int final_exit_code; // besides 0, an exit that means "don't relaunch me" (-1: none)
    int keep_fd; // an fd the child keeps across exec (-1: none)
    int keep_fd2; // second inherited fd for X11 socket activation (-1: none)
    void (*exited)(struct vela_child *child, int status);
    struct wl_event_loop *loop;
    char *command; // NULL: none
    pid_t pid;
    int pidfd;
    struct wl_event_source *source;
    int64_t started_ns;
    int quick_crashes; // abnormal exits in a row shortly after starting
};

void vela_child_init(struct vela_child *child, const char *name, int death_signal, int final_exit_code);
// False when it couldn't be started or watched.
bool vela_child_start(struct vela_child *child, struct wl_event_loop *loop, const char *command);
// We are shutting down: the child going away must not be relaunched.
void vela_child_stop(struct vela_child *child);

#endif
