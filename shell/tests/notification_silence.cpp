// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Do not disturb by itself while the screen is shared (src/notifications.h,
// docs/screen-sharing.md §7): popups held back in the notification center,
// critical ones still shown, the user's own Do not disturb never written.

#include "notifications.h"
#include "privatehome.h"

#include <QGuiApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

using namespace vela::test;

namespace {

uint notify(NotificationServer& server, const QString& summary, bool critical = false)
{
    QVariantMap hints;
    if (critical) {
        hints.insert(QStringLiteral("urgency"), 2);
    }
    return server.Notify(QStringLiteral("Mail"), 0, QString(), summary, QString(), {}, hints, 5000);
}

int historyCount(NotificationServer& server)
{
    return server.history()->property("count").toInt();
}

} // namespace

class NotificationSilenceTest : public QObject {
    Q_OBJECT

private slots:
    void heldBackWhileSharing()
    {
        NotificationServer server;
        QSignalSpy silenced(&server, &NotificationServer::silencedChanged);
        notify(server, QStringLiteral("before"));
        QCOMPARE(server.count(), 1); // a popup

        server.setAutomaticDoNotDisturb(true);
        QCOMPARE(silenced.count(), 1);
        QVERIFY(server.silenced());
        QVERIFY(!server.doNotDisturb());
        QCOMPARE(server.count(), 0); // the one shown moved to the center
        notify(server, QStringLiteral("during"));
        QCOMPARE(server.count(), 0); // no new popup...
        QCOMPARE(historyCount(server), 2); // ...it waits in the center
        // A critical one still pops up, as with Do not disturb.
        notify(server, QStringLiteral("critical"), true);
        QCOMPARE(server.count(), 1);

        server.setAutomaticDoNotDisturb(false);
        QCOMPARE(silenced.count(), 2);
        QVERIFY(!server.silenced());
        notify(server, QStringLiteral("after"));
        QCOMPARE(server.count(), 2);
    }

    // What is on screen when sharing starts would be shared too.
    void popupsShownWhenSharingStartsLeave()
    {
        NotificationServer server;
        QSignalSpy closed(&server, &NotificationServer::NotificationClosed);
        const uint normal = notify(server, QStringLiteral("normal"));
        server.Notify(QStringLiteral("Mail"), 0, QString(), QStringLiteral("never expires"), QString(), {}, {}, 0);
        const uint transient = server.Notify(QStringLiteral("Mail"), 0, QString(), QStringLiteral("transient"),
            QString(), {}, { { QStringLiteral("transient"), true } }, 5000);
        notify(server, QStringLiteral("critical"), true);
        QCOMPARE(server.count(), 4);

        server.setAutomaticDoNotDisturb(true);
        QCOMPARE(server.count(), 1); // the critical one stays
        QCOMPARE(historyCount(server), 2); // the others wait in the center...
        QCOMPARE(closed.count(), 1); // ...but the transient one, closed
        QCOMPARE(closed.first().first().toUInt(), transient);
        QVERIFY(normal != transient);
    }

    // A replacement while silenced: one entry, the same id, no popup left.
    void replacementsWhileSilenced()
    {
        NotificationServer server;
        QSignalSpy closed(&server, &NotificationServer::NotificationClosed);
        const uint shown = notify(server, QStringLiteral("download 10%"));
        server.setAutomaticDoNotDisturb(true);
        QCOMPARE(server.count(), 0);
        QCOMPARE(historyCount(server), 1);
        // In the center: replaced there, by the same id, again and again.
        const QVariantMap hints;
        QCOMPARE(server.Notify(QStringLiteral("Mail"), shown, QString(), QStringLiteral("download 50%"), QString(), {},
                     hints, 5000),
            shown);
        QCOMPARE(server.Notify(QStringLiteral("Mail"), shown, QString(), QStringLiteral("download 90%"), QString(), {},
                     hints, 5000),
            shown);
        QCOMPARE(historyCount(server), 1);
        auto* history = qobject_cast<QAbstractItemModel*>(server.history());
        QCOMPARE(history->data(history->index(0, 0), NotificationServer::SummaryRole).toString(),
            QStringLiteral("download 90%"));
        server.CloseNotification(shown);
        QCOMPARE(historyCount(server), 0);

        // A critical popup on screen, replaced by a normal one while silenced:
        // the old popup goes, the replacement waits in the center.
        server.setAutomaticDoNotDisturb(false);
        const uint critical = notify(server, QStringLiteral("battery 5%"), true);
        server.setAutomaticDoNotDisturb(true);
        QCOMPARE(server.count(), 1);
        closed.clear();
        QCOMPARE(server.Notify(QStringLiteral("Power"), critical, QString(), QStringLiteral("charging"), QString(), {},
                     hints, 5000),
            critical);
        QCOMPARE(server.count(), 0);
        QCOMPARE(historyCount(server), 1);
        QCOMPARE(closed.count(), 0); // replaced, not closed
    }

    void theUsersChoiceIsTheirs()
    {
        NotificationServer server;
        server.setDoNotDisturb(true);
        server.setAutomaticDoNotDisturb(true);
        server.setAutomaticDoNotDisturb(false);
        QVERIFY(server.doNotDisturb()); // still the user's
        QVERIFY(server.silenced());
        server.setDoNotDisturb(false);
        server.setAutomaticDoNotDisturb(true);
        QVERIFY(!server.doNotDisturb());
        // Never remembered as the user's choice.
        QCOMPARE(QSettings().value(QStringLiteral("notifications/doNotDisturb"), false).toBool(), false);
        server.setAutomaticDoNotDisturb(false);
        QVERIFY(!server.silenced());
    }

    // x-vela-expires: critical, so it passes Do not disturb, but it expires
    // into the notification center like others (a peripheral's battery,
    // docs/battery-notifications.md §5). Critical alone stays.
    void criticalThatExpires()
    {
        NotificationServer server;
        server.setDoNotDisturb(true);
        const QVariantMap expires { { QStringLiteral("urgency"), 2 }, { QStringLiteral("x-vela-expires"), true } };
        server.Notify(QStringLiteral("Devices"), 0, QString(), QStringLiteral("expires"), QString(), {}, expires, 200);
        server.Notify(QStringLiteral("Mail"), 0, QString(), QStringLiteral("stays"), QString(), {},
            { { QStringLiteral("urgency"), 2 } }, 100);
        QCOMPARE(server.count(), 2);
        QTRY_COMPARE(server.count(), 1);
        QCOMPARE(historyCount(server), 1);
        QTest::qWait(300);
        QCOMPARE(server.count(), 1); // 100 ms asked, critical: stays
    }

    // Too many popups: the oldest goes into the notification center, but
    // not a critical one that stays (the PC's battery,
    // docs/battery-notifications.md §5); it goes only when they all are.
    void criticalStaysWhenFull()
    {
        NotificationServer server;
        const uint battery = notify(server, QStringLiteral("battery"), true);
        const QVariantMap expires { { QStringLiteral("urgency"), 2 }, { QStringLiteral("x-vela-expires"), true } };
        server.Notify(QStringLiteral("Devices"), 0, QString(), QStringLiteral("mouse"), QString(), {}, expires, 5000);
        for (int i = 0; i < 4; ++i) {
            notify(server, QStringLiteral("mail %1").arg(i));
        }
        QCOMPARE(server.count(), 4);
        QCOMPARE(historyCount(server), 2);
        QCOMPARE(server.index(0).data(NotificationServer::IdRole).toUInt(), battery);
        QCOMPARE(server.index(1).data(NotificationServer::SummaryRole).toString(), QStringLiteral("mail 1"));
        for (int i = 0; i < 4; ++i) {
            notify(server, QStringLiteral("critical %1").arg(i), true);
        }
        QCOMPARE(server.count(), 4);
        QCOMPARE(server.index(0).data(NotificationServer::SummaryRole).toString(), QStringLiteral("critical 0"));
    }
};

int main(int argc, char** argv)
{
    if (!isolate()) {
        return 1;
    }
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("vela-shell"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    NotificationSilenceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "notification_silence.moc"
