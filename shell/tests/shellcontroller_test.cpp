// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shellcontroller.h"

#include <QGuiApplication>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTest>
#include <QTemporaryDir>
#include <QScreen>

class ShellControllerTest : public QObject {
    Q_OBJECT
private slots:
    void activeOutputSnapshot()
    {
        QTemporaryDir runtime;
        QVERIFY(runtime.isValid());
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("WAYLAND_DISPLAY", "shell-controller-test");
        ShellController shell;
        QVERIFY(shell.listen());
        QSignalSpy changes(&shell, &ShellController::activeScreenChanged);
        QStringList seen;
        connect(&shell, &ShellController::volumeKeyPressed, this, [&](const QString&) {
            seen.append(shell.activeScreen());
        });
        const auto send = [&](const QByteArray& message) {
            QLocalSocket socket;
            socket.connectToServer(ShellController::socketPath());
            QVERIFY(socket.waitForConnected(1000));
            QCOMPARE(socket.write(message), message.size());
            QVERIFY(socket.waitForBytesWritten(1000));
            socket.disconnectFromServer();
        };

        QCOMPARE(shell.panelScreen(), shell.primaryScreen());
        send("active-output DP-2\nvolume up\n");
        QTRY_COMPARE(seen.size(), 1);
        QCOMPARE(seen.last(), QStringLiteral("DP-2"));
        QCOMPARE(changes.size(), 1);
        // The compositor can announce an output before Qt sees it, or one
        // that was just removed. A stale name must not prevent placement.
        QCOMPARE(shell.panelScreen(), shell.primaryScreen());
        send("active-output DP-2\nvolume down\n");
        QTRY_COMPARE(seen.size(), 2);
        QCOMPARE(changes.size(), 1);
        send("active-output DP-1\nvolume up\n");
        QTRY_COMPARE(seen.size(), 3);
        QCOMPARE(seen.last(), QStringLiteral("DP-1"));
        QCOMPARE(changes.size(), 2);
        send("active-output \nvolume up\n");
        QTRY_COMPARE(seen.size(), 4);
        QVERIFY(seen.last().isEmpty());
        QCOMPARE(changes.size(), 3);
        QCOMPARE(shell.panelScreen(), shell.primaryScreen());
        const QString connected = shell.primaryScreen();
        if (!connected.isEmpty()) {
            send("active-output " + connected.toUtf8() + "\nvolume up\n");
            QTRY_COMPARE(seen.size(), 5);
            QCOMPARE(shell.panelScreen(), connected);
        }
    }
};

QTEST_MAIN(ShellControllerTest)
#include "shellcontroller_test.moc"
