// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// What a client on the session bus can make the shell show and keep
// (src/notifications.h): body markup reduced to <b>, <i>, <u> and safe links,
// summary and body bounded, the notification center capped.

#include "notifications.h"
#include "privatehome.h"

#include <QGuiApplication>
#include <QSignalSpy>
#include <QTest>

using namespace vela::test;

class NotificationLimitsTest : public QObject {
    Q_OBJECT

private slots:
    void bodyMarkup_data()
    {
        QTest::addColumn<QString>("body");
        QTest::addColumn<QString>("shown");
        QTest::newRow("plain text") << QStringLiteral("Hello & bye") << QStringLiteral("Hello & bye");
        QTest::newRow("spec styles") << QStringLiteral("<b>bold</b> <i>it</i> <u>u</u><br/>")
                                     << QStringLiteral("<b>bold</b> <i>it</i> <u>u</u><br/>");
        QTest::newRow("upper case") << QStringLiteral("<B>x</B>") << QStringLiteral("<b>x</b>");
        QTest::newRow("image") << QStringLiteral("<img src=\"http://host/a.png\"/>")
                               << QStringLiteral("&lt;img src=\"http://host/a.png\"/&gt;");
        QTest::newRow("other tag") << QStringLiteral("<script>x</script>")
                                   << QStringLiteral("&lt;script&gt;x&lt;/script&gt;");
        QTest::newRow("lone bracket") << QStringLiteral("a < b") << QStringLiteral("a &lt; b");
        QTest::newRow("https link") << QStringLiteral("<a href=\"https://example.com/?a=1&amp;b=2\">x</a>")
                                    << QStringLiteral("<a href=\"https://example.com/?a=1&amp;b=2\">x</a>");
        QTest::newRow("mailto link") << QStringLiteral("<a href='mailto:me@example.com'>me</a>")
                                     << QStringLiteral("<a href=\"mailto:me@example.com\">me</a>");
        QTest::newRow("file link") << QStringLiteral("<a href=\"file:///etc/passwd\">x</a>")
                                   << QStringLiteral("&lt;a href=\"file:///etc/passwd\"&gt;x");
        QTest::newRow("script link") << QStringLiteral("<a href=\"javascript:alert(1)\">x</a>")
                                     << QStringLiteral("&lt;a href=\"javascript:alert(1)\"&gt;x");
        QTest::newRow("unclosed link") << QStringLiteral("<a href=\"https://a.example\">x")
                                       << QStringLiteral("<a href=\"https://a.example\">x</a>");
        QTest::newRow("link with extra attributes")
            << QStringLiteral("<a href=\"https://a.example\" style=\"x\">x</a>")
            << QStringLiteral("&lt;a href=\"https://a.example\" style=\"x\"&gt;x");
    }

    void bodyMarkup()
    {
        QFETCH(QString, body);
        QFETCH(QString, shown);
        QCOMPARE(sanitizeNotificationBody(body), shown);
    }

    void textIsBounded()
    {
        NotificationServer server;
        server.Notify(QStringLiteral("Mail"), 0, QString(), QString(20000, QLatin1Char('s')),
            QString(20000, QLatin1Char('b')), {}, {}, 5000);
        QCOMPARE(server.count(), 1);
        const QModelIndex row = server.index(0, 0);
        QCOMPARE(server.data(row, NotificationServer::SummaryRole).toString().size(), maxNotificationSummaryChars);
        QCOMPARE(server.data(row, NotificationServer::BodyRole).toString().size(), maxNotificationBodyChars);
    }

    void centerIsCapped()
    {
        NotificationServer server;
        server.setAutomaticDoNotDisturb(true); // every notification goes to the center
        QSignalSpy closed(&server, &NotificationServer::NotificationClosed);
        const int extra = 5;
        uint first = 0;
        for (int i = 0; i < maxNotificationHistory + extra; ++i) {
            const uint id = server.Notify(QStringLiteral("Spam"), 0, QString(), QString::number(i), QString(), {}, {}, 5000);
            if (i == 0) {
                first = id;
            }
        }
        QCOMPARE(server.history()->property("count").toInt(), maxNotificationHistory);
        QCOMPARE(closed.count(), extra);
        QCOMPARE(closed.first().at(0).toUInt(), first); // the oldest left first
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
    NotificationLimitsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "notification_limits.moc"
