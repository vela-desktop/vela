// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "launcherentry.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QSignalSpy>
#include <QTest>

class LauncherEntryTest : public QObject {
    Q_OBJECT

private slots:
    void updatesAndUnregistration()
    {
        LauncherEntry entry;
        QSignalSpy changes(&entry, &LauncherEntry::changed);
        const QString connectionName = QStringLiteral("launcher-entry-test-sender");
        {
            const auto sender = QDBusConnection::connectToBus(QDBusConnection::SessionBus, connectionName);
            QVERIFY(sender.isConnected());

            const auto publish = [&](const QVariantMap& properties) {
                auto signal = QDBusMessage::createSignal(QStringLiteral("/Test"),
                    QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"));
                signal << QStringLiteral("application://com.example.Mail.desktop") << properties;
                QVERIFY(sender.send(signal));
            };
            publish({ { "count", QVariant::fromValue<qlonglong>(142) }, { "progress", 0.4 },
                { "progress-visible", true }, { "urgent", true } });
            QTRY_COMPARE(changes.count(), 1);
            const auto initial = entry.badge(QStringLiteral("mail.desktop"));
            QCOMPARE(initial.count, 142);
            QCOMPARE(initial.progress, 0.4);
            QVERIFY(initial.urgent);

            publish({ { "count-visible", false }, { "urgent", false }, { "progress", 1.0 } });
            QTRY_COMPARE(changes.count(), 2);
            QCOMPARE(entry.badge(QStringLiteral("com.example.Mail.desktop")).count, 0);
            QCOMPARE(entry.badge(QStringLiteral("mail.desktop")).progress, 1.0);
            QVERIFY(!entry.badge(QStringLiteral("mail.desktop")).urgent);

            publish({ { "count", QStringLiteral("wrong type") }, { "count-visible", true } });
            QTRY_COMPARE(changes.count(), 3);
            QCOMPARE(entry.badge(QStringLiteral("mail.desktop")).count, 142);
            QCOMPARE(entry.badge(QStringLiteral("unrelated.desktop")).count, 0);
            // Another publisher's app with the same last part is another app.
            QCOMPARE(entry.badge(QStringLiteral("org.other.Mail.desktop")).count, 0);
            QCOMPARE(entry.badge(QStringLiteral("org.other.Mail.desktop")).progress, -1.0);
        }
        QDBusConnection::disconnectFromBus(connectionName);
        QTRY_COMPARE(changes.count(), 4);
        QCOMPARE(entry.badge(QStringLiteral("mail.desktop")).count, 0);
        QCOMPARE(entry.badge(QStringLiteral("mail.desktop")).progress, -1.0);
    }
};

QTEST_GUILESS_MAIN(LauncherEntryTest)
#include "launcherentry_test.moc"
