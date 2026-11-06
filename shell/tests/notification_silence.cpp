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
        notify(server, QStringLiteral("during"));
        QCOMPARE(server.count(), 1); // no new popup...
        QCOMPARE(historyCount(server), 1); // ...it waits in the center
        // A critical one still pops up, as with Do not disturb.
        notify(server, QStringLiteral("critical"), true);
        QCOMPARE(server.count(), 2);

        server.setAutomaticDoNotDisturb(false);
        QCOMPARE(silenced.count(), 2);
        QVERIFY(!server.silenced());
        notify(server, QStringLiteral("after"));
        QCOMPARE(server.count(), 3);
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
