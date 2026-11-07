// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Real child processes: no private SDF setting leaks, but a user override does.
#include "processlaunch.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

namespace {
constexpr auto setting = "QSG_DISTANCEFIELD_ANTIALIASING";
constexpr auto retained = "VELA_PROCESS_LAUNCH_TEST_KEEP";

bool childIsCorrect(const QString& expected)
{
    const bool aa = expected == QLatin1String("unset")
        ? !qEnvironmentVariableIsSet(setting)
        : qEnvironmentVariableIsSet(setting) && qEnvironmentVariable(setting) == expected;
    return aa && qgetenv(retained) == "retained";
}

bool launchChildren(const QString& expected)
{
    vela::process::ExternalProcess child;
    child.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--probe"), expected });
    if (!child.waitForStarted(5000) || !child.waitForFinished(5000)
        || child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0) {
        return false;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return false;
    }
    const QString path = directory.filePath(QStringLiteral("probe"));
    if (!vela::process::startDetached(QCoreApplication::applicationFilePath(),
            { QStringLiteral("--probe-file"), expected, path })) {
        return false;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 5000) {
        QFile output(path);
        if (output.open(QIODevice::ReadOnly)) {
            const QByteArray result = output.readAll();
            if (!result.isEmpty()) {
                return result == "ok";
            }
        }
        QThread::msleep(10);
    }
    return false;
}

bool checkCase(const QString& expected)
{
    qunsetenv(retained);
    vela::process::setPrivateEnvironmentDefault(setting, "gray");
    vela::process::setPrivateEnvironmentDefault(setting, "gray"); // idempotence
    // Variables set after policy initialization must reach new children too.
    qputenv(retained, "retained");
    const QString parentValue = QString::fromUtf8(qgetenv(setting));
    const bool correctParent = expected == QLatin1String("unset")
        ? parentValue == QLatin1String("gray")
        : parentValue == expected;
    if (!correctParent || !launchChildren(expected)) {
        std::fprintf(stderr, "FAIL: child process environment (%s)\n", qPrintable(expected));
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 3 && args[1] == QLatin1String("--probe")) {
        return childIsCorrect(args[2]) ? 0 : 1;
    }
    if (args.size() >= 4 && args[1] == QLatin1String("--probe-file")) {
        QFile output(args[3]);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return 1;
        }
        const bool good = childIsCorrect(args[2]);
        output.write(good ? "ok" : "bad");
        return good ? 0 : 1;
    }
    if (args.size() >= 3 && args[1] == QLatin1String("--override-parent")) {
        return checkCase(args[2]) ? 0 : 1;
    }

    qunsetenv(setting);
    if (!checkCase(QStringLiteral("unset"))) {
        return 1;
    }

    // Later environment changes must survive: launchers cannot use a startup
    // snapshot, which would discard updated Wayland activation tokens too.
    qputenv(setting, "subpixel");
    if (!launchChildren(QStringLiteral("subpixel"))) {
        std::fprintf(stderr, "FAIL: later environment change was discarded\n");
        return 1;
    }

    // Fresh parents distinguish user-supplied values from injected defaults.
    for (const auto& value : { QStringLiteral("subpixel"), QString() }) {
        QProcess overridden;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QString::fromLatin1(setting), value);
        environment.insert(QString::fromLatin1(retained), QStringLiteral("retained"));
        overridden.setProcessEnvironment(environment);
        overridden.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--override-parent"), value });
        if (!overridden.waitForStarted(5000) || !overridden.waitForFinished(15000)
            || overridden.exitStatus() != QProcess::NormalExit || overridden.exitCode() != 0) {
            std::fprintf(stderr, "FAIL: user override did not survive external launch\n");
            return 1;
        }
    }
    return 0;
}
