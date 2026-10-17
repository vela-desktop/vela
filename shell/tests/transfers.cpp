// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "transfermonitor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QSignalSpy>
#include <QtTest>

class TransferEndpoint : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.vela.Files.Transfers")
public:
    int cancelledId = -1;
public Q_SLOTS:
    Q_SCRIPTABLE void Cancel(int id) { cancelledId = id; }
};

class Transfers : public QObject {
    Q_OBJECT
private slots:
    void progressesAndCompletesOnlyOnce()
    {
        TransferMonitor monitor;
        QSignalSpy completed(&monitor, &TransferMonitor::completed);

        monitor.onChanged(QStringLiteral(":1.21"), 7, QStringLiteral("Copying videos"),
            10, 100, QStringLiteral("clip.mkv"), false, {});
        QCOMPARE(monitor.activeCount(), 1);
        QCOMPARE(monitor.jobs().size(), 1);
        QCOMPARE(monitor.jobs().first().toMap().value(QStringLiteral("done")).toInt(), 10);

        monitor.onChanged(QStringLiteral(":1.21"), 7, QStringLiteral("Copying videos"),
            80, 100, QStringLiteral("clip.mkv"), false, {});
        QCOMPARE(monitor.activeCount(), 1);
        QCOMPARE(monitor.jobs().first().toMap().value(QStringLiteral("done")).toInt(), 80);

        monitor.onChanged(QStringLiteral(":1.21"), 7, QStringLiteral("Copying videos"),
            100, 100, {}, true, {});
        QCOMPARE(monitor.activeCount(), 0);
        QCOMPARE(completed.size(), 1);
        monitor.onChanged(QStringLiteral(":1.21"), 7, QStringLiteral("Copying videos"),
            100, 100, {}, true, {});
        QCOMPARE(completed.size(), 1);
        monitor.onRemoved(QStringLiteral(":1.21"), 7);
        QVERIFY(monitor.jobs().isEmpty());
    }

    void restoredResultsDoNotNotifyAgain()
    {
        TransferMonitor monitor;
        QSignalSpy completed(&monitor, &TransferMonitor::completed);
        monitor.onSnapshot(QStringLiteral(":1.20"), 4, QStringLiteral("Past copy"),
            100, 100, {}, true, {});
        QCOMPARE(monitor.activeCount(), 0);
        QCOMPARE(completed.size(), 0);
        monitor.onChanged(QStringLiteral(":1.20"), 4, QStringLiteral("Past copy"),
            100, 100, {}, true, {});
        QCOMPARE(completed.size(), 0);
    }

    void oldCompletedJobsDoNotBlockNewOnes()
    {
        TransferMonitor monitor;
        for (int i = 1; i <= 128; ++i) {
            monitor.onSnapshot(QStringLiteral(":1.20"), i, QStringLiteral("Past copy"),
                100, 100, {}, true, QStringLiteral("Failed"));
        }
        QCOMPARE(monitor.jobs().size(), 128);
        monitor.onChanged(QStringLiteral(":1.21"), 1000, QStringLiteral("New copy"),
            1, 100, {}, false, {});
        QCOMPARE(monitor.jobs().size(), 128);
        QCOMPARE(monitor.activeCount(), 1);
    }

    void distinctExplorerProcesses()
    {
        TransferMonitor monitor;
        monitor.onChanged(QStringLiteral(":1.30"), 1, QStringLiteral("Copying A"),
            1, 10, {}, false, {});
        monitor.onChanged(QStringLiteral(":1.31"), 1, QStringLiteral("Copying B"),
            2, 10, {}, false, {});
        QCOMPARE(monitor.activeCount(), 2);

        monitor.onRemoved(QStringLiteral(":1.30"), 1);
        QCOMPARE(monitor.activeCount(), 1);
        QCOMPARE(monitor.jobs().first().toMap().value(QStringLiteral("owner")).toString(),
            QStringLiteral(":1.31"));
    }

    void deadExplorerDoesNotLeaveAStaleIndicator()
    {
        TransferMonitor monitor;
        QSignalSpy completed(&monitor, &TransferMonitor::completed);
        monitor.onChanged(QStringLiteral(":1.40"), 3, QStringLiteral("Copying"),
            2, 10, {}, false, {});
        monitor.onOwnerChanged(QStringLiteral(":1.40"), QStringLiteral(":1.40"), {});
        QCOMPARE(monitor.activeCount(), 0);
        QVERIFY(monitor.jobs().isEmpty());
        QCOMPARE(completed.size(), 1);
        QVERIFY(!completed.first().at(1).toString().isEmpty());
    }

    void receivesRealSessionBusSignal()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) QSKIP("Session D-Bus unavailable");
        TransferMonitor monitor;
        QDBusMessage message = QDBusMessage::createSignal(QStringLiteral("/org/vela/Files/Transfers"),
            QStringLiteral("org.vela.Files.Transfers"), QStringLiteral("Changed"));
        message << bus.baseService() << 42 << QStringLiteral("Copying")
                << qlonglong(1) << qlonglong(100) << QStringLiteral("clip.mkv") << false << QString();
        QVERIFY(bus.send(message));
        QTRY_COMPARE(monitor.activeCount(), 1);
        QCOMPARE(monitor.jobs().first().toMap().value(QStringLiteral("owner")).toString(), bus.baseService());
    }

    void remoteCancellationReachesExplorerEndpoint()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) QSKIP("Session D-Bus unavailable");
        TransferEndpoint endpoint;
        const QString path = QStringLiteral("/org/vela/Files/Transfers");
        QVERIFY(bus.registerObject(path, &endpoint, QDBusConnection::ExportScriptableSlots));
        TransferMonitor monitor;
        monitor.onChanged(bus.baseService(), 93, QStringLiteral("Copying"),
            1, 100, {}, false, {});
        monitor.cancel(bus.baseService(), 93);
        QTRY_COMPARE(endpoint.cancelledId, 93);
        bus.unregisterObject(path);
    }

    void snapshotOverDbusDoesNotNotify()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) QSKIP("Session D-Bus unavailable");
        TransferMonitor monitor;
        QSignalSpy completed(&monitor, &TransferMonitor::completed);
        QDBusMessage message = QDBusMessage::createSignal(QStringLiteral("/org/vela/Files/Transfers"),
            QStringLiteral("org.vela.Files.Transfers"), QStringLiteral("Snapshot"));
        message << bus.baseService() << 94 << QStringLiteral("Old copy")
                << qlonglong(100) << qlonglong(100) << QString() << true << QString();
        QVERIFY(bus.send(message));
        QTRY_COMPARE(monitor.jobs().size(), 1);
        QCOMPARE(completed.size(), 0);
    }

    void ignoresMalformedJobs()
    {
        TransferMonitor monitor;
        monitor.onChanged(QStringLiteral("org.vela.fake"), -1, QStringLiteral("fake"),
            -1, -1, {}, false, {});
        QVERIFY(monitor.jobs().isEmpty());
    }
};

QTEST_GUILESS_MAIN(Transfers)
#include "transfers.moc"
