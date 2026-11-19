// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The notification area (src/tray.h) on the private bus of dbus-run-session:
// the shell's own StatusNotifierWatcher, and fake items that register with it
// from connections of their own, as apps do. What the taskbar reads from it:
// each icon's key, its state, and the right click's menu.

#include "tray.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusVirtualObject>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTest>

// The ToolTip property: (icon name, pictures, title, text), a picture
// being (width, height, ARGB bytes).
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

// A com.canonical.dbusmenu entry: (id, properties, children as variants).
struct MenuNode {
    int id = 0;
    QVariantMap properties;
    QList<MenuNode> children;
};
Q_DECLARE_METATYPE(MenuNode)

QDBusArgument& operator<<(QDBusArgument& argument, const MenuNode& node)
{
    argument.beginStructure();
    argument << node.id << node.properties;
    argument.beginArray(QMetaType::fromType<QDBusVariant>());
    for (const MenuNode& child : node.children) {
        argument << QDBusVariant(QVariant::fromValue(child));
    }
    argument.endArray();
    argument.endStructure();
    return argument;
}
const QDBusArgument& operator>>(const QDBusArgument& argument, MenuNode&)
{
    return argument;
}

namespace {

const QString watcherService = QStringLiteral("org.kde.StatusNotifierWatcher");
const QString watcherPath = QStringLiteral("/StatusNotifierWatcher");
const QString itemInterface = QStringLiteral("org.kde.StatusNotifierItem");

// An app's StatusNotifierItem: GetAll answers `properties`. Its menu, at
// the Menu path, answers GetLayout with `menu`; without one, it fails.
class FakeItem : public QDBusVirtualObject {
public:
    FakeItem(const QString& name, const QVariantMap& properties, const QList<MenuNode>& menu = {})
        : m_name(name)
        , m_connection(QDBusConnection::connectToBus(QDBusConnection::SessionBus, name))
        , m_properties(properties)
        , m_menu(menu)
    {
        m_connection.registerVirtualObject(QStringLiteral("/StatusNotifierItem"), this);
        m_connection.registerVirtualObject(QStringLiteral("/MenuBar"), this);
    }
    ~FakeItem() override
    {
        m_connection.unregisterObject(QStringLiteral("/StatusNotifierItem"));
        m_connection.unregisterObject(QStringLiteral("/MenuBar"));
        QDBusConnection::disconnectFromBus(m_name);
    }

    // As libappindicator (only the path) or as KDE apps (the bus name).
    void registerWith(const QString& what)
    {
        QDBusMessage call = QDBusMessage::createMethodCall(
            watcherService, watcherPath, watcherService, QStringLiteral("RegisterStatusNotifierItem"));
        call << what;
        m_connection.asyncCall(call);
    }
    QString unique() const { return m_connection.baseService(); }
    void set(const QString& name, const QVariant& value)
    {
        m_properties.insert(name, value);
        m_connection.send(QDBusMessage::createSignal(
                              QStringLiteral("/StatusNotifierItem"), itemInterface, QStringLiteral("NewStatus"))
            << m_properties.value(QStringLiteral("Status")).toString());
    }

    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection&) override
    {
        if (message.member() == QLatin1String("GetAll")) {
            m_connection.send(message.createReply(QVariant::fromValue(m_properties)));
            return true;
        }
        if (message.member() == QLatin1String("GetLayout") && !m_menu.isEmpty()) {
            const MenuNode root { 0, {}, m_menu };
            m_connection.send(message.createReply(QVariantList { 1u, QVariant::fromValue(root) }));
            return true;
        }
        m_connection.send(message.createErrorReply(QDBusError::UnknownMethod, message.member()));
        return true;
    }

private:
    QString m_name;
    QDBusConnection m_connection;
    QVariantMap m_properties;
    QList<MenuNode> m_menu;
};

QVariantMap properties(const QString& id, const QString& title, const QString& status = QStringLiteral("Active"))
{
    return {
        { QStringLiteral("Id"), id },
        { QStringLiteral("Title"), title },
        { QStringLiteral("Status"), status },
        { QStringLiteral("IconName"), QStringLiteral("dialog-information") },
        { QStringLiteral("Menu"), QVariant::fromValue(QDBusObjectPath(QStringLiteral("/MenuBar"))) },
    };
}

QVariantMap entryFor(const TrayModel& tray, const QString& key)
{
    for (const QVariant& entry : tray.entries()) {
        if (entry.toMap().value(QStringLiteral("key")) == key) {
            return entry.toMap();
        }
    }
    return {};
}

} // namespace

class TrayTest : public QObject {
    Q_OBJECT

    // One for the whole test: the watcher's bus name stays taken.
    TrayModel* m_tray = nullptr;

private slots:
    void initTestCase()
    {
        qDBusRegisterMetaType<Picture>();
        qDBusRegisterMetaType<ToolTip>();
        qDBusRegisterMetaType<MenuNode>();
        m_tray = new TrayModel(this);
        m_tray->start();
    }
    // Each case's fake apps have quit: the list is empty again.
    void cleanup() { QTRY_COMPARE_WITH_TIMEOUT(m_tray->entries().size(), 0, 3000); }

    void keysAndStates()
    {
        TrayModel& tray = *m_tray;
        QSignalSpy entries(&tray, &TrayModel::entriesChanged);

        // Id is the key; without one, the Title, even when the tooltip
        // names it otherwise.
        FakeItem steam(QStringLiteral("steam"), properties(QStringLiteral("steam"), QStringLiteral("Steam")));
        QVariantMap discord = properties(QString(), QStringLiteral("Discord"));
        discord.insert(QStringLiteral("ToolTip"), QVariant::fromValue(ToolTip { QStringLiteral("Discord: 3 unread") }));
        FakeItem untitled(QStringLiteral("untitled"), discord);
        FakeItem quiet(QStringLiteral("quiet"),
            properties(QStringLiteral("updates"), QStringLiteral("Updates"), QStringLiteral("Passive")));
        steam.registerWith(QStringLiteral("/StatusNotifierItem"));
        untitled.registerWith(untitled.unique());
        quiet.registerWith(quiet.unique() + QStringLiteral("/StatusNotifierItem"));
        QTRY_COMPARE_WITH_TIMEOUT(tray.entries().size(), 3, 3000);
        QVERIFY(entries.count() > 0);

        const QVariantMap first = entryFor(tray, QStringLiteral("steam"));
        QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("Steam"));
        QVERIFY(!first.value(QStringLiteral("attention")).toBool());
        QVERIFY(!first.value(QStringLiteral("passive")).toBool());
        QCOMPARE(entryFor(tray, QStringLiteral("Discord")).value(QStringLiteral("title")).toString(),
            QStringLiteral("Discord: 3 unread"));
        // A passive icon is listed, for the drawer to leave out.
        const QVariantMap passive = entryFor(tray, QStringLiteral("updates"));
        QVERIFY(passive.value(QStringLiteral("passive")).toBool());
        // The apps register from their own connections, in no set order.
        const int passiveRow = passive.value(QStringLiteral("row")).toInt();
        QCOMPARE(tray.data(tray.index(passiveRow), TrayModel::IconRole).toString(), QString());

        // Its state follows the app.
        steam.set(QStringLiteral("Status"), QStringLiteral("NeedsAttention"));
        QTRY_VERIFY_WITH_TIMEOUT(
            entryFor(tray, QStringLiteral("steam")).value(QStringLiteral("attention")).toBool(), 3000);
        // Each entry's row is the model's.
        for (const QVariant& entry : tray.entries()) {
            const int row = entry.toMap().value(QStringLiteral("row")).toInt();
            QCOMPARE(tray.data(tray.index(row), TrayModel::TitleRole), entry.toMap().value(QStringLiteral("title")));
        }
    }

    void anAppGoingAwayLeavesTheList()
    {
        TrayModel& tray = *m_tray;
        {
            FakeItem item(QStringLiteral("brief"), properties(QStringLiteral("brief"), QStringLiteral("Brief")));
            item.registerWith(QStringLiteral("/StatusNotifierItem"));
            QTRY_COMPARE_WITH_TIMEOUT(tray.entries().size(), 1, 3000);
        }
        QSignalSpy entries(&tray, &TrayModel::entriesChanged);
        QTRY_COMPARE_WITH_TIMEOUT(tray.entries().size(), 0, 3000);
        QVERIFY(entries.count() > 0);
    }

    void theRightClickMenu()
    {
        TrayModel& tray = *m_tray;
        QVariantMap noMenu = properties(QStringLiteral("plain"), QStringLiteral("Plain"));
        noMenu.remove(QStringLiteral("Menu"));
        FakeItem plain(QStringLiteral("plain"), noMenu);
        FakeItem broken(QStringLiteral("broken"), properties(QStringLiteral("broken"), QStringLiteral("Broken")));
        plain.registerWith(QStringLiteral("/StatusNotifierItem"));
        QTRY_COMPARE_WITH_TIMEOUT(tray.entries().size(), 1, 3000);
        broken.registerWith(QStringLiteral("/StatusNotifierItem"));
        QTRY_COMPARE_WITH_TIMEOUT(tray.entries().size(), 2, 3000);
        QSignalSpy menus(&tray, &TrayModel::menuReady);

        // No menu of the app's: only a right click asks for one anyway, and
        // gets it at once, empty, for the shell's own entries.
        tray.requestMenu(0, 40);
        QTest::qWait(200);
        QCOMPARE(menus.count(), 0);
        tray.requestMenu(0, 40, true);
        QCOMPARE(menus.count(), 1);
        QCOMPARE(menus.at(0).at(0).toInt(), 0);
        QCOMPARE(menus.at(0).at(1).toInt(), 40);
        QCOMPARE(menus.at(0).at(2).toList(), QVariantList());

        // A menu that fails to load: the same.
        tray.requestMenu(1, 80);
        QTest::qWait(300);
        QCOMPARE(menus.count(), 1);
        tray.requestMenu(1, 80, true);
        QTRY_COMPARE_WITH_TIMEOUT(menus.count(), 2, 3000);
        QCOMPARE(menus.at(1).at(0).toInt(), 1);
        QCOMPARE(menus.at(1).at(2).toList(), QVariantList());

        // No such icon: nothing.
        tray.requestMenu(7, 0, true);
        QTest::qWait(100);
        QCOMPARE(menus.count(), 2);
    }

    // The app's own menu, as it sends it, with or without a right click.
    void theAppsMenu()
    {
        TrayModel& tray = *m_tray;
        const QList<MenuNode> menu {
            { 1, { { QStringLiteral("label"), QStringLiteral("_Open") } }, {} },
            { 2, { { QStringLiteral("type"), QStringLiteral("separator") } }, {} },
            { 3, { { QStringLiteral("label"), QStringLiteral("Hidden") }, { QStringLiteral("visible"), false } }, {} },
            { 4, { { QStringLiteral("label"), QStringLiteral("Status") } },
                { { 5,
                    { { QStringLiteral("label"), QStringLiteral("Away") },
                        { QStringLiteral("toggle-type"), QStringLiteral("radio") },
                        { QStringLiteral("toggle-state"), 1 } },
                    {} } } },
        };
        FakeItem app(QStringLiteral("menu-app"), properties(QStringLiteral("menu-app"), QStringLiteral("App")), menu);
        app.registerWith(QStringLiteral("/StatusNotifierItem"));
        QTRY_COMPARE_WITH_TIMEOUT(tray.entries().size(), 1, 3000);
        QSignalSpy menus(&tray, &TrayModel::menuReady);
        for (const bool always : { false, true }) {
            menus.clear();
            tray.requestMenu(0, 10, always);
            QTRY_COMPARE_WITH_TIMEOUT(menus.count(), 1, 3000);
            const QVariantList entries = menus.at(0).at(2).toList();
            QCOMPARE(entries.size(), 3); // the hidden one left out
            QCOMPARE(entries.at(0).toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Open"));
            QVERIFY(entries.at(1).toMap().value(QStringLiteral("separator")).toBool());
            const QVariantList status = entries.at(2).toMap().value(QStringLiteral("children")).toList();
            QCOMPARE(status.size(), 1);
            QVERIFY(status.at(0).toMap().value(QStringLiteral("radio")).toBool());
            QVERIFY(status.at(0).toMap().value(QStringLiteral("checked")).toBool());
        }
    }

    // Neither an Id nor a Title: the tooltip's title is the key.
    void theTooltipNamesAnUntitledIcon()
    {
        TrayModel& tray = *m_tray;
        QVariantMap bare = properties(QString(), QString());
        bare.insert(QStringLiteral("ToolTip"), QVariant::fromValue(ToolTip { QStringLiteral("Backups") }));
        FakeItem item(QStringLiteral("bare"), bare);
        item.registerWith(QStringLiteral("/StatusNotifierItem"));
        QTRY_COMPARE_WITH_TIMEOUT(tray.entries().size(), 1, 3000);
        QCOMPARE(tray.entries().at(0).toMap().value(QStringLiteral("key")).toString(), QStringLiteral("Backups"));
    }

    // The key is trimmed, as the saved list is; one taken from a title
    // stays while the title counts messages; an Id, once there, wins.
    void keysStayWhatTheChoiceWasSavedBy()
    {
        TrayModel& tray = *m_tray;
        FakeItem padded(QStringLiteral("padded"), properties(QStringLiteral("  steam  "), QStringLiteral("Steam")));
        FakeItem inbox(QStringLiteral("inbox"), properties(QString(), QStringLiteral(" Inbox (3) ")));
        padded.registerWith(QStringLiteral("/StatusNotifierItem"));
        inbox.registerWith(QStringLiteral("/StatusNotifierItem"));
        QTRY_COMPARE_WITH_TIMEOUT(tray.entries().size(), 2, 3000);
        QVERIFY(!entryFor(tray, QStringLiteral("steam")).isEmpty());
        QVERIFY(!entryFor(tray, QStringLiteral("Inbox (3)")).isEmpty());

        inbox.set(QStringLiteral("Title"), QStringLiteral("Inbox (4)"));
        QTRY_COMPARE_WITH_TIMEOUT(entryFor(tray, QStringLiteral("Inbox (3)")).value(QStringLiteral("title")).toString(),
            QStringLiteral("Inbox (4)"), 3000);
        QVERIFY(entryFor(tray, QStringLiteral("Inbox (4)")).isEmpty());

        inbox.set(QStringLiteral("Id"), QStringLiteral("mail"));
        QTRY_VERIFY_WITH_TIMEOUT(!entryFor(tray, QStringLiteral("mail")).isEmpty(), 3000);
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    TrayTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tray_test.moc"
