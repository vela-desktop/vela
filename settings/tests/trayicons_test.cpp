// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings' list of notification area icons (src/trayicons.h) against a fake
// StatusNotifierWatcher and fake items, each on its own connection to the
// private bus of dbus-run-session.

#include "trayicons.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QSignalSpy>
#include <QTest>

// The ToolTip property: (icon name, pictures, title, text).
struct Picture {
    int width = 0;
    int height = 0;
    QByteArray bytes;
};
Q_DECLARE_METATYPE(Picture)

QDBusArgument& operator<<(QDBusArgument& argument, const Picture& picture)
{
    argument.beginStructure();
    argument << picture.width << picture.height << picture.bytes;
    argument.endStructure();
    return argument;
}
const QDBusArgument& operator>>(const QDBusArgument& argument, Picture&)
{
    return argument;
}

struct ToolTip {
    QString title;
};
Q_DECLARE_METATYPE(ToolTip)

QDBusArgument& operator<<(QDBusArgument& argument, const ToolTip& tip)
{
    argument.beginStructure();
    argument << QString();
    argument.beginArray(QMetaType::fromType<Picture>());
    argument.endArray();
    argument << tip.title << QString();
    argument.endStructure();
    return argument;
}
const QDBusArgument& operator>>(const QDBusArgument& argument, ToolTip&)
{
    return argument;
}

namespace {

const QString watcherService = QStringLiteral("org.kde.StatusNotifierWatcher");
const QString watcherPath = QStringLiteral("/StatusNotifierWatcher");
const QString propertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

// Answers the Properties calls of one object: `properties` for Get and GetAll.
class FakeObject : public QDBusVirtualObject {
public:
    QVariantMap properties;
    QDBusConnection connection { QString() };

    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection&) override
    {
        if (message.interface() != propertiesInterface) {
            return false;
        }
        if (message.member() == QLatin1String("Get")) {
            const QString name = message.arguments().value(1).toString();
            connection.send(message.createReply(QVariant::fromValue(QDBusVariant(properties.value(name)))));
        } else if (message.member() == QLatin1String("GetAll")) {
            connection.send(message.createReply(QVariant::fromValue(properties)));
        } else {
            return false;
        }
        return true;
    }
};

// A connection of its own, so each fake has its own unique name.
struct Peer {
    QString name;
    QDBusConnection connection;
    FakeObject object;

    Peer(const QString& name, const QString& path, const QVariantMap& properties)
        : name(name)
        , connection(QDBusConnection::connectToBus(QDBusConnection::SessionBus, name))
    {
        object.properties = properties;
        object.connection = connection;
        connection.registerVirtualObject(path, &object);
    }
    ~Peer()
    {
        connection.unregisterObject(QStringLiteral("/"), QDBusConnection::UnregisterTree);
        QDBusConnection::disconnectFromBus(name);
    }
    QString unique() const { return connection.baseService(); }
};

QVariantMap item(const QString& id, const QString& title, const QString& icon)
{
    QVariantMap properties;
    if (!id.isEmpty()) {
        properties.insert(QStringLiteral("Id"), id);
    }
    if (!title.isEmpty()) {
        properties.insert(QStringLiteral("Title"), title);
    }
    properties.insert(QStringLiteral("IconName"), icon);
    return properties;
}

QVariantMap entry(const QString& key, const QString& title, const QString& icon)
{
    return { { QStringLiteral("key"), key }, { QStringLiteral("title"), title }, { QStringLiteral("icon"), icon } };
}

} // namespace

class TrayIconsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qDBusRegisterMetaType<Picture>();
        qDBusRegisterMetaType<ToolTip>();
    }

    void emptyWithoutAWatcher()
    {
        TrayIcons icons;
        QSignalSpy changed(&icons, &TrayIcons::itemsChanged);
        // The shell is not running: no answer, no icons, nothing to tell.
        QTest::qWait(300);
        QCOMPARE(icons.items(), QVariantList());
        QCOMPARE(changed.count(), 0);
    }

    void listsTheRunningIcons()
    {
        const QString itemPath = QStringLiteral("/StatusNotifierItem");
        Peer steam(QStringLiteral("steam"), itemPath,
            item(QStringLiteral("steam"), QStringLiteral("Steam"), QStringLiteral("steam")));
        // Without an Id the title is the key, as in the shell.
        Peer discord(QStringLiteral("discord"), itemPath, item(QString(), QStringLiteral("Discord"), QString()));
        Peer nameless(QStringLiteral("nameless"), QStringLiteral("/org/ayatana/NotificationItem/x"),
            item(QStringLiteral("x-tray"), QString(), QStringLiteral("x")));
        // A second copy of the same app is one icon.
        Peer again(QStringLiteral("again"), itemPath,
            item(QStringLiteral("steam"), QStringLiteral("Steam 2"), QStringLiteral("steam")));
        Peer blank(QStringLiteral("blank"), itemPath, item(QString(), QString(), QStringLiteral("blank")));

        Peer watcher(QStringLiteral("watcher"), watcherPath, {});
        QVERIFY(watcher.connection.interface()->registerService(watcherService).isValid());
        watcher.object.properties.insert(QStringLiteral("RegisteredStatusNotifierItems"),
            QStringList {
                steam.unique() + itemPath,
                discord.unique(), // only the service: the default path
                nameless.unique() + QStringLiteral("/org/ayatana/NotificationItem/x"), again.unique() + itemPath,
                blank.unique() + itemPath,
                QStringLiteral(":1.9999/StatusNotifierItem"), // gone
            });

        TrayIcons icons;
        QSignalSpy changed(&icons, &TrayIcons::itemsChanged);
        QVERIFY(changed.wait(3000));
        const QVariantList expected {
            entry(QStringLiteral("steam"), QStringLiteral("Steam"), QStringLiteral("steam")),
            entry(QStringLiteral("Discord"), QStringLiteral("Discord"), QStringLiteral("application-x-executable")),
            entry(QStringLiteral("x-tray"), QStringLiteral("x-tray"), QStringLiteral("x")),
        };
        QCOMPARE(icons.items(), expected);

        // The same answer again changes nothing.
        icons.refresh();
        QTest::qWait(300);
        QCOMPARE(changed.count(), 1);

        // One goes away: the watcher says so, the list follows.
        watcher.object.properties.insert(
            QStringLiteral("RegisteredStatusNotifierItems"), QStringList { discord.unique() + itemPath });
        QDBusMessage gone =
            QDBusMessage::createSignal(watcherPath, watcherService, QStringLiteral("StatusNotifierItemUnregistered"));
        gone << steam.unique() + itemPath;
        QVERIFY(watcher.connection.send(gone));
        QTRY_COMPARE_WITH_TIMEOUT(changed.count(), 2, 3000);
        QCOMPARE(icons.items(), (QVariantList { expected.at(1) }));

        // And none left.
        watcher.object.properties.insert(QStringLiteral("RegisteredStatusNotifierItems"), QStringList());
        QDBusMessage registered =
            QDBusMessage::createSignal(watcherPath, watcherService, QStringLiteral("StatusNotifierItemRegistered"));
        registered << QString();
        QVERIFY(watcher.connection.send(registered));
        QTRY_COMPARE_WITH_TIMEOUT(changed.count(), 3, 3000);
        QCOMPARE(icons.items(), QVariantList());
        watcher.connection.interface()->unregisterService(watcherService);
    }

    // The shell's keys: trimmed, and the tooltip's title when there is no
    // Id nor Title. The shell going away empties the list.
    void sameKeysAsTheShell()
    {
        const QString itemPath = QStringLiteral("/StatusNotifierItem");
        Peer padded(QStringLiteral("padded"), itemPath,
            item(QStringLiteral(" steam "), QStringLiteral("Steam"), QStringLiteral("steam")));
        QVariantMap bare = item(QString(), QString(), QStringLiteral("backup"));
        bare.insert(QStringLiteral("ToolTip"), QVariant::fromValue(ToolTip { QStringLiteral("Backups") }));
        Peer backups(QStringLiteral("backups"), itemPath, bare);
        Peer watcher(QStringLiteral("watcher3"), watcherPath, {});
        QVERIFY(watcher.connection.interface()->registerService(watcherService).isValid());
        watcher.object.properties.insert(QStringLiteral("RegisteredStatusNotifierItems"),
            QStringList { padded.unique() + itemPath, backups.unique() + itemPath });

        TrayIcons icons;
        QSignalSpy changed(&icons, &TrayIcons::itemsChanged);
        QVERIFY(changed.wait(3000));
        const QVariantList expected {
            entry(QStringLiteral("steam"), QStringLiteral("Steam"), QStringLiteral("steam")),
            entry(QStringLiteral("Backups"), QStringLiteral("Backups"), QStringLiteral("backup")),
        };
        QCOMPARE(icons.items(), expected);

        watcher.connection.interface()->unregisterService(watcherService);
        QTRY_COMPARE_WITH_TIMEOUT(icons.items(), QVariantList(), 3000);
    }

    void aNewerRefreshWins()
    {
        const QString itemPath = QStringLiteral("/StatusNotifierItem");
        Peer first(QStringLiteral("first"), itemPath, item(QStringLiteral("first"), QString(), QString()));
        Peer watcher(QStringLiteral("watcher2"), watcherPath, {});
        QVERIFY(watcher.connection.interface()->registerService(watcherService).isValid());
        watcher.object.properties.insert(
            QStringLiteral("RegisteredStatusNotifierItems"), QStringList { first.unique() + itemPath });

        TrayIcons icons;
        // Refreshed again before the first answer: only the last one counts.
        icons.refresh();
        icons.refresh();
        QSignalSpy changed(&icons, &TrayIcons::itemsChanged);
        QVERIFY(changed.wait(3000));
        QTest::qWait(300);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(icons.items().size(), 1);
        watcher.connection.interface()->unregisterService(watcherService);
    }
};

QTEST_GUILESS_MAIN(TrayIconsTest)
#include "trayicons_test.moc"
