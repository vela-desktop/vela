// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "launcherentry.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QSignalSpy>
#include <QTest>

#include <limits>

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

    // Signals that are not an app's badge change nothing.
    void ignoresWhatIsNotABadge()
    {
        LauncherEntry entry;
        QSignalSpy changes(&entry, &LauncherEntry::changed);
        const QString connectionName = QStringLiteral("launcher-entry-test-ignored");
        {
            const auto sender = QDBusConnection::connectToBus(QDBusConnection::SessionBus, connectionName);
            QVERIFY(sender.isConnected());
            const auto publish = [&](const QString& uri, const QVariantMap& properties) {
                auto signal = QDBusMessage::createSignal(QStringLiteral("/Test"),
                    QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"));
                signal << uri << properties;
                QVERIFY(sender.send(signal));
            };
            const QVariantMap count { { "count", QVariant::fromValue<qlonglong>(3) }, { "count-visible", true } };
            publish(QStringLiteral("file:///usr/share/applications/mail.desktop"), count);
            publish(QStringLiteral("application://"), count);
            publish(QStringLiteral("application://.desktop"), count);
            publish(QStringLiteral("application://../mail.desktop"), count);
            publish(QStringLiteral("application://mail.desktop?x"), count);
            publish(QStringLiteral("application://mail.desktop#x"), count);
            // No property of a badge, or only of the wrong types.
            publish(QStringLiteral("application://mail.desktop"), { { "count", QStringLiteral("3") } });
            publish(QStringLiteral("application://mail.desktop"),
                { { "progress", std::numeric_limits<double>::quiet_NaN() }, { "urgent", 1 } });
            // Then a real one: only it arrives.
            publish(QStringLiteral("application://other.desktop"), count);
            QTRY_COMPARE(changes.count(), 1);
            QTest::qWait(100);
            QCOMPARE(changes.count(), 1);
            QCOMPARE(entry.badge(QStringLiteral("mail.desktop")).count, 0);
            QCOMPARE(entry.badge(QStringLiteral("other.desktop")).count, 3);
            QCOMPARE(entry.badge(QString()).count, 0);
            QCOMPARE(entry.badge(QStringLiteral(".desktop")).progress, -1.0);
        }
        QDBusConnection::disconnectFromBus(connectionName);
        QTRY_COMPARE(changes.count(), 2);
    }

    // Several apps and publishers: one leaving takes only its own badges.
    void onePublisherLeaving()
    {
        LauncherEntry entry;
        QSignalSpy changes(&entry, &LauncherEntry::changed);
        const auto connect = [](const QString& name) {
            return QDBusConnection::connectToBus(QDBusConnection::SessionBus, name);
        };
        const auto publish = [](const QDBusConnection& bus, const QString& uri, qlonglong count) {
            auto signal = QDBusMessage::createSignal(
                QStringLiteral("/Test"), QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"));
            signal << uri << QVariantMap { { "count", QVariant::fromValue(count) } };
            QVERIFY(bus.send(signal));
        };
        {
            const auto first = connect(QStringLiteral("launcher-entry-first"));
            const auto second = connect(QStringLiteral("launcher-entry-second"));
            publish(first, QStringLiteral("application://mail.desktop"), 1);
            publish(first, QStringLiteral("application://chat.desktop"), 2);
            publish(second, QStringLiteral("application://news.desktop"), 4);
            QTRY_COMPARE(changes.count(), 3);
        }
        QDBusConnection::disconnectFromBus(QStringLiteral("launcher-entry-second"));
        QTRY_COMPARE(changes.count(), 4);
        QCOMPARE(entry.badge(QStringLiteral("news.desktop")).count, 0);
        QCOMPARE(entry.badge(QStringLiteral("mail.desktop")).count, 1);
        QCOMPARE(entry.badge(QStringLiteral("chat.desktop")).count, 2);
        QDBusConnection::disconnectFromBus(QStringLiteral("launcher-entry-first"));
        QTRY_COMPARE(changes.count(), 5);
        QCOMPARE(entry.badge(QStringLiteral("chat.desktop")).count, 0);
    }
};

QTEST_GUILESS_MAIN(LauncherEntryTest)
#include "launcherentry_test.moc"
