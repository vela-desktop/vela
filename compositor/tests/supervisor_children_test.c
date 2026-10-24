// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Check /proc enumeration and SIGTERM/SIGKILL without dynamic PID arrays.
#include "supervisor_children.h"

#undef NDEBUG // CI uses RelWithDebInfo: assertions must remain active.
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILDREN 20

static void check_signaled(pid_t pid, int expected)
{
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == expected);
}

int main(void)
{
    assert(vela_supervisor_children(0) == 0);
    pid_t children[CHILDREN];
    for (int i = 0; i < CHILDREN; ++i) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            for (;;) {
                pause();
            }
        }
        children[i] = pid;
    }
    assert(vela_supervisor_children(0) == CHILDREN);
    assert(vela_supervisor_children(SIGTERM) == CHILDREN);
    for (int i = 0; i < CHILDREN; ++i) {
        check_signaled(children[i], SIGTERM);
    }
    assert(vela_supervisor_children(0) == 0);

    // Escalate for a child that ignores SIGTERM, as the supervisor does.
    int ready[2];
    assert(pipe(ready) == 0);
    pid_t stubborn = fork();
    assert(stubborn >= 0);
    if (stubborn == 0) {
        close(ready[0]);
        signal(SIGTERM, SIG_IGN);
        assert(write(ready[1], "1", 1) == 1);
        close(ready[1]);
        for (;;) {
            pause();
        }
    }
    close(ready[1]);
    char byte;
    assert(read(ready[0], &byte, 1) == 1);
    close(ready[0]);
    assert(vela_supervisor_children(SIGTERM) == 1);
    assert(vela_supervisor_children(0) == 1);
    assert(vela_supervisor_children(SIGKILL) == 1);
    check_signaled(stubborn, SIGKILL);
    assert(vela_supervisor_children(0) == 0);
    return 0;
}
