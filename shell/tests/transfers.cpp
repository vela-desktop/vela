// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "transfermonitor.h"

#include <QSignalSpy>
#include <QtTest>

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
