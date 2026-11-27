// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The buses and daemons a test starts end with it. A test that aborts
// otherwise leaves them running, and the daemon, which writes to the
// test's stderr, keeps CTest waiting for the test's whole timeout.

#pragma once

#include <QProcess>

#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>

inline void endWithThisTest(QProcess& process)
{
    const pid_t test = getpid();
    process.setChildProcessModifier([test] {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        // The test may have ended before prctl.
        if (getppid() != test) {
            _exit(1);
        }
    });
}
