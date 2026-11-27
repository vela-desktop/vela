// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The UI executable the test build of the daemon trusts is the test's own
// (bluetooth/CMakeLists.txt). Under Valgrind (docs/ci-lanes.md) the
// daemon reads Valgrind's tool in /proc/PID/exe instead: the test names it
// in VELA_BT_TRUSTED_EXE, which only the test build reads. Valgrind answers
// the test's own readlink with the test's path, so a child, not run under
// Valgrind, reads it.

#pragma once

#include "undervalgrind.h"

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>

inline void trustThisTest(QProcessEnvironment& environment)
{
    if (!VELA_UNDER_VALGRIND()) {
        return;
    }
    QProcess readlink;
    readlink.start(
        QStringLiteral("readlink"), { QStringLiteral("/proc/%1/exe").arg(QCoreApplication::applicationPid()) });
    if (readlink.waitForFinished() && readlink.exitCode() == 0) {
        environment.insert(
            QStringLiteral("VELA_BT_TRUSTED_EXE"), QString::fromLocal8Bit(readlink.readAllStandardOutput()).trimmed());
    }
}
