// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-updates-client (common/updates, docs/updates.md §8, §10) against the
// real vela-update-daemon and the fakes: the last check as QML reads it,
// failures with their reason, an install through the terminal, the
// automatic checks switch shown at once, the announcement, refusals in
// plain words, and the daemon missing, leaving and coming back.

#include "harness.h"
#include "updates/updates.h"

#include <QSignalSpy>

#include <memory>

using vela::updates::Updates;

class UpdatesClient : public QObject, protected UpdatesBench {
    Q_OBJECT

public:
    UpdatesClient()
        : UpdatesBench(QStringLiteral("client"))
    {
    }

private:
    // A client, as Settings makes it, once it has read the daemon.
    std::unique_ptr<Updates> client()
    {
        auto updates = std::make_unique<Updates>(bus());
        [&] { QTRY_VERIFY(!updates->source().isEmpty()); }();
        return updates;
    }

private Q_SLOTS:
    void initTestCase()
    {
        const QString error = setUp();
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void init() { reset(); }
    void cleanup() { release(); }
    void cleanupTestCase() { tearDown(); }

    // Without the daemon: as before any check, and nothing to ask.
    void daemonMissing()
    {
        Updates updates(bus());
        QTest::qWait(100);
        QVERIFY(!updates.available());
        QCOMPARE(updates.status(), QStringLiteral("unknown"));
        QCOMPARE(updates.activity(), QStringLiteral("idle"));
        QVERIFY(!updates.busy());
        QVERIFY(!updates.lastCheck().isValid());
        QVERIFY(updates.checkError().isEmpty());
        updates.check();
        updates.setAutoCheck(false);
        QVERIFY(updates.autoCheck());
        QVERIFY(updates.error().isEmpty());
        QCOMPARE(calls(), QStringList {});
    }

    void checkFindsAnUpdate()
    {
        nextCheck({ { "status", "update-available" }, { "commit", commitB }, { "version", "0.1.1" },
            { "changes", QJsonArray { QJsonObject { { "commit", commitB }, { "summary", "Faster blur" } },
                             QJsonObject { { "commit", commitA }, { "summary", "Fix the dock" } } } },
            { "changes_total", 140 } });
        QVERIFY(startDaemon());
        auto updates = client();
        QVERIFY(updates->available());
        QCOMPARE(updates->status(), QStringLiteral("unknown"));
        QVERIFY(updates->autoCheck());

        QVERIFY(write(fake(QStringLiteral("check-hold")), ""));
        updates->check();
        QTRY_COMPARE(updates->activity(), QStringLiteral("checking"));
        QVERIFY(updates->busy());
        QFile::remove(fake(QStringLiteral("check-hold")));
        QTRY_COMPARE(updates->activity(), QStringLiteral("idle"));
        QCOMPARE(updates->status(), QStringLiteral("update-available"));
        QCOMPARE(updates->channel(), QStringLiteral("experimental"));
        QCOMPARE(updates->installedPackage(), QStringLiteral("vela-git"));
        QCOMPARE(updates->installedVersion(), QStringLiteral("0.1.0.r1.gaaaaaaa-1"));
        QCOMPARE(updates->version(), QStringLiteral("0.1.1"));
        QCOMPARE(updates->commit(), commitB);
        QCOMPARE(updates->changesTotal(), 140);
        QCOMPARE(updates->changes().size(), 2);
        const QVariantMap first = updates->changes().value(0).toMap();
        QCOMPARE(first.value(QStringLiteral("commit")).toString(), commitB);
        QCOMPARE(first.value(QStringLiteral("summary")).toString(), QStringLiteral("Faster blur"));
        QVERIFY(updates->lastCheck().isValid());
        QVERIFY(updates->lastCheck() > QDateTime::currentDateTime().addSecs(-1100));
        QVERIFY(updates->checkError().isEmpty());
        QCOMPARE(updates->announce(), commitB);
    }

    // Beta's release texts, and the reason while it has no release (§12).
    void betaNotesAndNoRelease()
    {
        nextCheck({ { "channel", "beta" }, { "status", "update-available" }, { "installed_package", "vela" },
            { "version", "0.1.11" }, { "tag", "v0.1.11" },
            { "notes", QJsonArray { QJsonObject { { "version", "0.1.11" }, { "text", "Faster" } },
                           QJsonObject { { "version", "0.1.10" }, { "text", "Fixes" } } } } });
        QVERIFY(startDaemon());
        auto updates = client();
        updates->check();
        QTRY_COMPARE(updates->status(), QStringLiteral("update-available"));
        QCOMPARE(updates->tag(), QStringLiteral("v0.1.11"));
        QCOMPARE(updates->notes(),
            (QVariantList { QVariantMap { { QStringLiteral("version"), QStringLiteral("0.1.11") }, { QStringLiteral("text"), QStringLiteral("Faster") } },
                QVariantMap { { QStringLiteral("version"), QStringLiteral("0.1.10") }, { QStringLiteral("text"), QStringLiteral("Fixes") } } }));
        QVERIFY(updates->changes().isEmpty());

        QTRY_VERIFY(!updates->busy());
        nextCheck({ { "channel", "beta" }, { "status", "error" }, { "installed_package", "vela" },
                      { "error", QJsonObject { { "code", "not-found" }, { "message", "no release" } } } },
            1);
        updates->check();
        QTRY_COMPARE(updates->checkError().value(QStringLiteral("code")).toString(), QStringLiteral("not-found"));
        QCOMPARE(updates->checkError().value(QStringLiteral("reason")).toString(), updates->reason(QStringLiteral("not-found")));
        QVERIFY(updates->reason(QStringLiteral("not-found")).contains(QStringLiteral("Beta")));
    }

    // A failed check leaves the last completed one, with the failure
    // beside it; the next completed check clears it.
    void failureWithItsReason()
    {
        nextCheck({ { "status", "update-available" }, { "commit", commitB } });
        QVERIFY(startDaemon());
        auto updates = client();
        updates->check();
        QTRY_COMPARE(updates->status(), QStringLiteral("update-available"));
        QTRY_VERIFY(!updates->busy());

        nextCheck({ { "status", "error" }, { "error", QJsonObject { { "code", "network" }, { "message", "offline" } } } }, 1);
        updates->check();
        QTRY_VERIFY(!updates->checkError().isEmpty());
        const QVariantMap failure = updates->checkError();
        QCOMPARE(failure.value(QStringLiteral("code")).toString(), QStringLiteral("network"));
        QCOMPARE(failure.value(QStringLiteral("message")).toString(), QStringLiteral("offline"));
        QCOMPARE(failure.value(QStringLiteral("reason")).toString(), updates->reason(QStringLiteral("network")));
        QVERIFY(updates->reason(QStringLiteral("network")) != updates->reason(QStringLiteral("failed")));
        QCOMPARE(updates->status(), QStringLiteral("update-available"));
        QCOMPARE(updates->commit(), commitB);

        QTRY_VERIFY(!updates->busy());
        nextCheck({});
        updates->check();
        QTRY_COMPARE(updates->status(), QStringLiteral("up-to-date"));
        QTRY_VERIFY(updates->checkError().isEmpty());
    }

    void refusalsInPlainWords()
    {
        nextCheck({});
        QVERIFY(write(fake(QStringLiteral("check-hold")), ""));
        QVERIFY(startDaemon());
        auto updates = client();
        QSignalSpy errors(updates.get(), &Updates::errorChanged);
        updates->check();
        QTRY_COMPARE(updates->activity(), QStringLiteral("checking"));
        updates->check();
        QTRY_VERIFY(!updates->error().isEmpty());
        const QString busy = updates->error();
        // Asking again clears it first.
        QFile::remove(fake(QStringLiteral("check-hold")));
        QTRY_VERIFY(!updates->busy());
        updates->install({ { QStringLiteral("branch"), QStringLiteral("a..b") } });
        QTRY_VERIFY(!updates->error().isEmpty() && updates->error() != busy);
        QVERIFY(updates->error().contains(QStringLiteral("branch")));
        QVERIFY(!QFile::exists(fake(QStringLiteral("terminal"))));
        updates->clearError();
        QVERIFY(updates->error().isEmpty());
        QVERIFY(errors.size() >= 4);
    }

    void installInTheTerminal()
    {
        QVERIFY(write(fake(QStringLiteral("terminal-runs")), ""));
        QVERIFY(startDaemon());
        auto updates = client();
        // Empty options are left out.
        updates->install({ { QStringLiteral("channel"), QStringLiteral("experimental") },
            { QStringLiteral("branch"), QStringLiteral("fix/nvidia") }, { QStringLiteral("commit"), QString() } });
        QTRY_COMPARE(updates->activity(), QStringLiteral("installing"));
        QTRY_COMPARE(updates->phase(), QStringLiteral("building"));
        QCOMPARE(QString::fromUtf8(read(fake(QStringLiteral("terminal")))).split(u'\n', Qt::SkipEmptyParts).mid(5),
            (QStringList { QStringLiteral("--channel"), QStringLiteral("experimental"), QStringLiteral("--branch"),
                QStringLiteral("fix/nvidia") }));
        QVERIFY(updates->error().isEmpty());

        QVERIFY(write(fake(QStringLiteral("install-result")), "failed"));
        QVERIFY(write(fake(QStringLiteral("install-release")), ""));
        QTRY_COMPARE(updates->activity(), QStringLiteral("idle"));
        QCOMPARE(updates->phase(), QString());
        QCOMPARE(updates->installResult(), QStringLiteral("failed"));
        QCOMPARE(updates->installError().value(QStringLiteral("code")).toString(), QStringLiteral("failed"));
        QCOMPARE(updates->installError().value(QStringLiteral("reason")).toString(), updates->reason(QStringLiteral("failed")));
    }

    void terminalThatNeverStarts()
    {
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_START_TIMEOUT_MS"), QStringLiteral("300") } }));
        auto updates = client();
        updates->install();
        QTRY_COMPARE(updates->activity(), QStringLiteral("starting"));
        QVERIFY(updates->busy());
        QTRY_COMPARE(updates->activity(), QStringLiteral("idle"));
        QCOMPARE(updates->installError().value(QStringLiteral("code")).toString(), QStringLiteral("terminal"));
        QCOMPARE(updates->installError().value(QStringLiteral("reason")).toString(), updates->reason(QStringLiteral("terminal")));
    }

    // The switch moves at once, and stays where the daemon put it.
    void automaticChecksSwitch()
    {
        QVERIFY(startDaemon());
        auto updates = client();
        QVERIFY(updates->autoCheck());
        updates->setAutoCheck(false);
        QVERIFY(!updates->autoCheck());
        QTRY_VERIFY(read(m_home.filePath(QStringLiteral("config/vela/vela.conf"))).contains("update-check=no"));
        QTest::qWait(100);
        QVERIFY(!updates->autoCheck());
        updates->setAutoCheck(true);
        QVERIFY(updates->autoCheck());
        QTRY_VERIFY(read(m_home.filePath(QStringLiteral("config/vela/vela.conf"))).contains("update-check=yes"));
    }

    void announcedOnce()
    {
        nextCheck({ { "status", "update-available" }, { "commit", commitB } });
        QVERIFY(startDaemon());
        auto updates = client();
        updates->check();
        QTRY_COMPARE(updates->announce(), commitB);
        updates->announced(commitB);
        QTRY_COMPARE(updates->announce(), QString());
        QVERIFY(updates->error().isEmpty());
    }

    // Leaving, the daemon takes what it knew; coming back, it's read again.
    void daemonLeavesAndComesBack()
    {
        nextCheck({ { "status", "update-available" }, { "commit", commitB } });
        QVERIFY(startDaemon());
        auto updates = client();
        updates->check();
        QTRY_COMPARE(updates->status(), QStringLiteral("update-available"));
        QTRY_VERIFY(!updates->busy());

        stopDaemon();
        QTRY_VERIFY(!updates->available());
        QCOMPARE(updates->status(), QStringLiteral("unknown"));
        QCOMPARE(updates->commit(), QString());
        QVERIFY(!updates->lastCheck().isValid());

        QVERIFY(startDaemon());
        QTRY_VERIFY(updates->available());
        QTRY_COMPARE(updates->status(), QStringLiteral("update-available"));
        QCOMPARE(updates->commit(), commitB);
    }
};

QTEST_GUILESS_MAIN(UpdatesClient)
#include "client.moc"
