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
constexpr auto activation = "XDG_ACTIVATION_TOKEN";

/** Check isolation and the activation token observed inside an actual child. */
bool childIsCorrect(const QString& expected, const QString& token = {})
{
    const bool aa = expected == QLatin1String("unset")
        ? !qEnvironmentVariableIsSet(setting)
        : qEnvironmentVariableIsSet(setting) && qEnvironmentVariable(setting) == expected;
    return aa && qgetenv(retained) == "retained"
        && (token.isNull() || qgetenv(activation) == token.toUtf8());
}

/** Wait for a real child and require a successful environment probe. */
bool finishedCorrectly(QProcess& child)
{
    return child.waitForStarted(5000) && child.waitForFinished(5000)
        && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0;
}

/** Wait for a detached probe's completed output rather than an empty open file. */
bool probeFileIsCorrect(const QString& path)
{
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

/** Verify both managed and detached helpers against the expected AA setting. */
bool launchChildren(const QString& expected)
{
    vela::process::ExternalProcess child;
    child.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--probe"), expected });
    if (!finishedCorrectly(child)) {
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
    return probeFileIsCorrect(path);
}

/** Reuse objects across environment changes through every exposed launch form. */
bool reusedProcessesAreCurrent()
{
    vela::process::ExternalProcess child;
    const QString program = QCoreApplication::applicationFilePath();
    for (int mode = 0; mode < 4; ++mode) {
        const QString token = QStringLiteral("updated-token-%1").arg(mode);
        qputenv(activation, token.toUtf8());
        const QString expected = mode == 0 ? QStringLiteral("unset") : QStringLiteral("subpixel");
        if (mode != 0) {
            qputenv(setting, "subpixel");
        }
        const QStringList arguments { QStringLiteral("--probe"), expected, token };
        child.setProgram(program);
        child.setArguments(arguments);
        switch (mode) {
        case 0: child.start(program, arguments); break;
        case 1: child.start(); break;
        case 2: child.startCommand(QStringLiteral("\"%1\" --probe %2 %3").arg(program, expected, token)); break;
        case 3:
            if (!static_cast<QIODevice*>(&child)->open(QIODevice::ReadWrite)) {
                return false;
            }
            break;
        }
        if (!finishedCorrectly(child)) {
            return false;
        }
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return false;
    }
    vela::process::ExternalProcess detached;
    detached.setProgram(program);
    for (int run = 0; run < 2; ++run) {
        const QString token = QStringLiteral("detached-token-%1").arg(run);
        qputenv(activation, token.toUtf8());
        const QString path = directory.filePath(QString::number(run));
        detached.setArguments({ QStringLiteral("--probe-file"), QStringLiteral("subpixel"), path, token });
        if (!detached.startDetached() || !probeFileIsCorrect(path)) {
            return false;
        }
    }
    return true;
}

/** Apply a private default twice and ensure explicit overrides survive child launches. */
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

/** Run isolated parent cases or answer child-process environment probes. */
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 3 && args[1] == QLatin1String("--probe")) {
        return childIsCorrect(args[2], args.size() >= 4 ? args[3] : QString()) ? 0 : 1;
    }
    if (args.size() >= 4 && args[1] == QLatin1String("--probe-file")) {
        QFile output(args[3]);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return 1;
        }
        const bool good = childIsCorrect(args[2], args.size() >= 5 ? args[4] : QString());
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
    if (!reusedProcessesAreCurrent()) {
        std::fprintf(stderr, "FAIL: reused process retained an old environment\n");
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
