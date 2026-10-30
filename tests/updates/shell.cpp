// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The shell's side of updates (docs/updates.md §9.2, §10) against the real
// vela-update-daemon and the fakes: a new update is a notification once,
// also across sessions, and its click opens Updates; a newer one is told
// again, being up to date isn't; "Sign out to finish updating" once the
// compositor was replaced, with "Sign out".

#include "harness.h"
#include "notifications.h"
#include "updatenotices.h"
#include "updates/updates.h"

#include <memory>

using vela::updates::Updates;

class UpdatesShell : public QObject, protected UpdatesBench {
    Q_OBJECT

public:
    UpdatesShell()
        : UpdatesBench(QStringLiteral("shell"))
    {
    }

private:
    // The shell: its client of the daemon, the notifications and what
    // goes between them.
    std::unique_ptr<Updates> updates;
    std::unique_ptr<NotificationServer> notifications;
    std::unique_ptr<UpdateNotices> notices;
    int opened = 0; // Settings > Updates
    int signedOut = 0;

    // `daemon`: wait until the client has read it.
    void startShell(bool daemon = true)
    {
        updates = std::make_unique<Updates>(bus());
        notifications = std::make_unique<NotificationServer>();
        notices = std::make_unique<UpdateNotices>(
            updates.get(), notifications.get(), [this] { ++opened; }, [this] { ++signedOut; });
        if (daemon) {
            [&] { QTRY_VERIFY(!updates->source().isEmpty()); }();
        }
    }

    void stopShell()
    {
        notices.reset();
        notifications.reset();
        updates.reset();
    }

    QVariant notificationData(int role, int row = 0)
    {
        return notifications->data(notifications->index(row), role);
    }

    QStringList actionKeys(int row = 0)
    {
        QStringList keys;
        for (const QVariant& action : notificationData(NotificationServer::ActionsRole, row).toList()) {
            keys << action.toMap().value(QStringLiteral("key")).toString();
        }
        return keys;
    }

private Q_SLOTS:
    void initTestCase()
    {
        const QString error = setUp();
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void init()
    {
        reset();
        opened = signedOut = 0;
    }

    void cleanup()
    {
        stopShell();
        release();
    }

    void cleanupTestCase() { tearDown(); }

    void anUpdateIsToldOnce()
    {
        nextCheck({ { "status", "update-available" }, { "commit", commitB }, { "version", "0.1.1" } });
        QVERIFY(startDaemon());
        startShell();
        QCOMPARE(notifications->count(), 0);
        updates->check();
        QTRY_COMPARE(notifications->count(), 1);
        QCOMPARE(notificationData(NotificationServer::SummaryRole).toString(),
            QStringLiteral("Vela 0.1.1 (bbbbbbb) is available"));
        QCOMPARE(actionKeys(), QStringList { QStringLiteral("open") });
        QVERIFY(notificationData(NotificationServer::HasDefaultActionRole).toBool());
        // Told: the daemon doesn't ask again.
        QTRY_COMPARE(updates->announce(), QString());
        QTest::qWait(200);
        QCOMPARE(notifications->count(), 1);

        // A click opens Settings > System > Updates.
        notifications->invoke(notices->updateNotification(), QStringLiteral("default"));
        QCOMPARE(opened, 1);
        QCOMPARE(notifications->count(), 0);

        // Not in the next session either.
        stopShell();
        stopDaemon();
        QVERIFY(startDaemon());
        startShell();
        QTest::qWait(300);
        QCOMPARE(notifications->count(), 0);
    }

    // A newer update is news again, in place of the older one's; being up
    // to date isn't.
    void aNewerUpdateIsToldAgain()
    {
        nextCheck({ { "status", "update-available" }, { "commit", commitB } });
        QVERIFY(startDaemon());
        startShell();
        updates->check();
        QTRY_COMPARE(notifications->count(), 1);
        const uint first = notices->updateNotification();
        QTRY_VERIFY(!updates->busy() && updates->announce().isEmpty());

        nextCheck({ { "status", "update-available" }, { "commit", commitA } });
        updates->check();
        QTRY_COMPARE(notificationData(NotificationServer::SummaryRole).toString(),
            QStringLiteral("Vela 0.1.0 (aaaaaaa) is available"));
        QCOMPARE(notifications->count(), 1);
        QCOMPARE(notices->updateNotification(), first);
        // "Open Updates" does what the click does.
        notifications->invoke(first, QStringLiteral("open"));
        QCOMPARE(opened, 1);

        QTRY_VERIFY(!updates->busy() && updates->announce().isEmpty());
        nextCheck({});
        updates->check();
        QTRY_COMPARE(updates->status(), QStringLiteral("up-to-date"));
        QTest::qWait(200);
        QCOMPARE(notifications->count(), 0);
    }

    void upToDateIsNotNews()
    {
        nextCheck({});
        QVERIFY(startDaemon());
        startShell();
        updates->check();
        QTRY_COMPARE(updates->status(), QStringLiteral("up-to-date"));
        QTest::qWait(200);
        QCOMPARE(notifications->count(), 0);
    }

    // Once the running compositor's file is replaced (a copy of sh here,
    // as in the daemon's test): once, with "Sign out".
    void signOutToFinish()
    {
        const QString copy = m_home.filePath(QStringLiteral("vela-compositor"));
        QVERIFY(QFile::copy(QStringLiteral("/bin/sh"), copy));
        QProcess compositor;
        compositor.start(copy, { QStringLiteral("-c"), QStringLiteral("read line") });
        QVERIFY(compositor.waitForStarted());
        nextCheck({});
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_SESSION_PID"), QString::number(compositor.processId()) } }));
        startShell();
        QVERIFY(!updates->needsRelogin());
        QVERIFY(QFile::remove(copy));
        updates->check();
        QTRY_VERIFY(updates->needsRelogin());
        QTRY_COMPARE(notifications->count(), 1);
        QCOMPARE(notificationData(NotificationServer::SummaryRole).toString(), QStringLiteral("Sign out to finish updating"));
        QCOMPARE(actionKeys(), QStringList { QStringLiteral("sign-out") });

        // Once, whatever else changes.
        QTRY_VERIFY(!updates->busy());
        updates->check();
        QTRY_VERIFY(!updates->busy());
        QTest::qWait(200);
        QCOMPARE(notifications->count(), 1);

        notifications->invoke(notices->reloginNotification(), QStringLiteral("sign-out"));
        QCOMPARE(signedOut, 1);
        QCOMPARE(opened, 0);
        compositor.kill();
        compositor.waitForFinished();
    }

    void daemonMissing()
    {
        startShell(false);
        QTest::qWait(200);
        QVERIFY(!updates->available());
        QCOMPARE(notifications->count(), 0);
    }
};

QTEST_GUILESS_MAIN(UpdatesShell)
#include "shell.moc"
