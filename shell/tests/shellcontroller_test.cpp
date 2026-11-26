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
        connect(&shell, &ShellController::volumeKeyPressed, this,
            [&](const QString&) { seen.append(shell.activeScreen()); });
        const auto send = [&](const QByteArray& message) {
            QLocalSocket socket;
            socket.connectToServer(ShellController::socketPath());
            QVERIFY(socket.waitForConnected(1000));
            QCOMPARE(socket.write(message), message.size());
            if (socket.bytesToWrite() > 0) {
                QVERIFY(socket.waitForBytesWritten(1000));
            }
            socket.disconnectFromServer();
        };

        QCOMPARE(shell.panelScreen(), shell.primaryScreen());
        send("active-output DP-2\tvolume up\n");
        QTRY_COMPARE(seen.size(), 1);
        QCOMPARE(seen.last(), QStringLiteral("DP-2"));
        QCOMPARE(changes.size(), 1);
        // The compositor can announce an output before Qt sees it, or one
        // that was just removed. A stale name must not prevent placement.
        QCOMPARE(shell.panelScreen(), shell.primaryScreen());
        send("active-output DP-2\tvolume down\n");
        QTRY_COMPARE(seen.size(), 2);
        QCOMPARE(changes.size(), 1);
        send("active-output DP-1\tvolume up\n");
        QTRY_COMPARE(seen.size(), 3);
        QCOMPARE(seen.last(), QStringLiteral("DP-1"));
        QCOMPARE(changes.size(), 2);
        send("active-output \tvolume up\n");
        QTRY_COMPARE(seen.size(), 4);
        QVERIFY(seen.last().isEmpty());
        QCOMPARE(changes.size(), 3);
        QCOMPARE(shell.panelScreen(), shell.primaryScreen());
        // An incomplete frame from one connection must not affect another.
        QLocalSocket partial;
        partial.connectToServer(ShellController::socketPath());
        QVERIFY(partial.waitForConnected(1000));
        const QByteArray firstHalf("active-output DP-2");
        QCOMPARE(partial.write(firstHalf), firstHalf.size());
        if (partial.bytesToWrite() > 0) {
            QVERIFY(partial.waitForBytesWritten(1000));
        }
        send("active-output DP-1\tvolume up\n");
        QTRY_COMPARE(seen.size(), 5);
        QCOMPARE(seen.last(), QStringLiteral("DP-1"));
        const QByteArray secondHalf("\tvolume down\n");
        QCOMPARE(partial.write(secondHalf), secondHalf.size());
        if (partial.bytesToWrite() > 0) {
            QVERIFY(partial.waitForBytesWritten(1000));
        }
        QTRY_COMPARE(seen.size(), 6);
        QCOMPARE(seen.last(), QStringLiteral("DP-2"));
        partial.disconnectFromServer();
        send("active-output DP-1\tvolume up"); // no newline: discard
        send("active-output DP-1\n"); // no command: discard
        send("active-output \tvolume up\n");
        QTRY_COMPARE(seen.size(), 7);
        QVERIFY(seen.last().isEmpty());
        const QString connected = shell.primaryScreen();
        if (!connected.isEmpty()) {
            send("active-output " + connected.toUtf8() + "\tvolume up\n");
            QTRY_COMPARE(seen.size(), 8);
            QCOMPARE(shell.panelScreen(), connected);
        }
    }
};

QTEST_MAIN(ShellControllerTest)
#include "shellcontroller_test.moc"
