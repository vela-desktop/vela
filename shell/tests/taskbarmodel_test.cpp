// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The taskbar's buttons (src/taskbarmodel.h) from windows no compositor
// backs: their events go through ForeignToplevel's own handlers, their
// requests are recorded, and so are the commands for the compositor. The
// apps are .desktop files in a private home, the badges come from another
// connection to the private bus of dbus-run-session.

#include "appmodel.h"
#include "config.h"
#include "foreigntoplevels.h"
#include "launcherentry.h"
#include "shellcontroller.h"
#include "taskbarmodel.h"
#include "workspaces.h"

#include "privatehome.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QWindow>

using namespace vela::test;

namespace {

QStringList requests; // "activate Mail", "minimize Mail"...
QByteArrayList commands; // to the compositor

} // namespace

// Records instead of opening the compositor's socket.
bool ShellController::sendToCompositor(const QByteArray& command)
{
    commands << command;
    return true;
}

class FakeWindow : public ForeignToplevel {
public:
    explicit FakeWindow(const QString& identifier)
        : ForeignToplevel(nullptr)
    {
        this->identifier = identifier;
    }

    // As the compositor describes it, then "done".
    void describe(const QString& title, const QString& appId, std::initializer_list<uint32_t> states = {})
    {
        zwlr_foreign_toplevel_handle_v1_title(title);
        zwlr_foreign_toplevel_handle_v1_app_id(appId);
        wl_array array { states.size() * sizeof(uint32_t), states.size() * sizeof(uint32_t),
            const_cast<uint32_t*>(std::data(states)) };
        zwlr_foreign_toplevel_handle_v1_state(&array);
        zwlr_foreign_toplevel_handle_v1_done();
    }
    void closeNow() { zwlr_foreign_toplevel_handle_v1_closed(); }

    void requestActivate() override { requests << QStringLiteral("activate ") + identifier; }
    void requestMinimize() override { requests << QStringLiteral("minimize ") + identifier; }
    void requestMaximize(bool on) override
    {
        requests << (on ? QStringLiteral("maximize ") : QStringLiteral("unmaximize ")) + identifier;
    }
    void requestClose() override { requests << QStringLiteral("close ") + identifier; }
};

class FakeWindows : public ForeignToplevelManager {
public:
    FakeWindow* open(const QString& identifier, const QString& title, const QString& appId,
        std::initializer_list<uint32_t> states = {})
    {
        auto* window = new FakeWindow(identifier);
        adopt(window, ++m_handle);
        window->describe(title, appId, states);
        return window;
    }

private:
    uint32_t m_handle = 0;
};

namespace {

constexpr uint32_t activated = ForeignToplevel::state_activated;
constexpr uint32_t minimized = ForeignToplevel::state_minimized;
constexpr uint32_t maximized = ForeignToplevel::state_maximized;

bool writeApp(const QString& id, const QByteArray& name, const QByteArray& extra = {})
{
    return writeFile(QStringLiteral("data/applications/") + id,
        "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=true\nIcon=" + name.toLower() + "\n" + extra);
}

bool writeConf(const QByteArray& taskbar)
{
    return writeFile(QStringLiteral("config/Vela/vela-shell.conf"), "[taskbar]\n" + taskbar);
}

QVariant role(const TaskbarModel& model, int row, TaskbarModel::Role which)
{
    return model.data(model.index(row), which);
}

QStringList names(const TaskbarModel& model)
{
    QStringList list;
    for (int row = 0; row < model.rowCount(); ++row) {
        list << role(model, row, TaskbarModel::NameRole).toString();
    }
    return list;
}

void publishBadge(const QDBusConnection& bus, const QString& id, const QVariantMap& properties)
{
    auto signal = QDBusMessage::createSignal(
        QStringLiteral("/Test"), QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"));
    signal << QStringLiteral("application://") + id << properties;
    QVERIFY(bus.send(signal));
}

} // namespace

class TaskbarModelTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(writeApp(QStringLiteral("mail.desktop"), "Mail"));
        QVERIFY(writeApp(QStringLiteral("org.example.Mail.desktop"), "Mail")); // its Flatpak
        QVERIFY(writeApp(QStringLiteral("editor.desktop"), "Editor"));
        QVERIFY(writeApp(QStringLiteral("player.desktop"), "Player"));
        QVERIFY(writeFile(QStringLiteral("launchers/backup.desktop"),
            "[Desktop Entry]\nType=Application\nName=Mail\nExec=true --backup\n"));
    }

    void init()
    {
        requests.clear();
        commands.clear();
        QSettings().remove(QStringLiteral("taskbar"));
        QVERIFY(writeConf({}));
    }

    // A window no compositor backs: its requests go nowhere, its events are
    // applied at "done".
    void aWindowWithNoHandle()
    {
        struct Bare : ForeignToplevel {
            Bare()
                : ForeignToplevel(nullptr)
            {
            }
            void describe(std::initializer_list<uint32_t> states)
            {
                wl_array array { states.size() * sizeof(uint32_t), states.size() * sizeof(uint32_t),
                    const_cast<uint32_t*>(std::data(states)) };
                zwlr_foreign_toplevel_handle_v1_state(&array);
                zwlr_foreign_toplevel_handle_v1_parent(nullptr);
                zwlr_foreign_toplevel_handle_v1_done();
            }
        } window;
        QVERIFY(!window.isInitialized());
        window.requestActivate();
        window.requestMinimize();
        window.requestMaximize(true);
        window.requestMaximize(false);
        window.requestClose();
        QWindow panel;
        window.setButtonRect(&panel, QRect(0, 0, 10, 10));
        window.setButtonRect(nullptr, {});

        QSignalSpy changes(&window, &ForeignToplevel::changed);
        window.describe({ activated, maximized, ForeignToplevel::state_fullscreen, 99 });
        QCOMPARE(changes.count(), 1);
        QVERIFY(window.ready && window.activated && window.maximized && window.fullscreen && !window.minimized);
        const quint64 first = window.lastActivated;
        window.describe({ minimized });
        QVERIFY(window.minimized && !window.activated && !window.maximized);
        window.describe({ activated });
        QVERIFY(window.lastActivated > first);
    }

    // Pinned apps first (one per name, those not installed left out), then
    // the open ones by app; dialogs stay under their window.
    void pinnedThenOpenApps()
    {
        AppModel apps;
        apps.reload();
        FakeWindows windows;
        TaskbarModel model(&apps, &windows);
        QCOMPARE(model.rowCount(), 0);

        model.setPinnedIds({ QStringLiteral("mail.desktop"), QStringLiteral("missing.desktop"),
            QStringLiteral("org.example.Mail.desktop"), QStringLiteral("editor.desktop") });
        QCOMPARE(names(model), QStringList({ QStringLiteral("Mail"), QStringLiteral("Editor") }));
        QCOMPARE(QSettings().value(QStringLiteral("taskbar/pinned")).toStringList(), model.pinnedIds());

        // The Flatpak's window goes on the pinned package's button.
        FakeWindow* mail = windows.open(QStringLiteral("w-mail"), QStringLiteral("Inbox"),
            QStringLiteral("org.example.Mail"), { activated, maximized });
        FakeWindow* dialog = windows.open(QStringLiteral("w-dialog"), QStringLiteral("Attach"), QStringLiteral("mail"));
        dialog->parentWindow = mail;
        emit dialog->changed();
        windows.open(QStringLiteral("w-term"), QStringLiteral("Terminal"), QStringLiteral("term"));
        windows.open(QStringLiteral("w-player"), QStringLiteral("Song"), QStringLiteral("player"));

        QCOMPARE(names(model),
            QStringList({ QStringLiteral("Mail"), QStringLiteral("Editor"), QStringLiteral("Terminal"),
                QStringLiteral("Player") }));
        QCOMPARE(role(model, 0, TaskbarModel::WindowCountRole).toInt(), 1);
        QVERIFY(role(model, 0, TaskbarModel::WindowActiveRole).toBool());
        QCOMPARE(role(model, 0, TaskbarModel::ActiveWindowIdRole).toString(), QStringLiteral("w-mail"));
        QCOMPARE(role(model, 0, TaskbarModel::WindowIdsRole).toStringList(), QStringList { QStringLiteral("w-mail") });
        QCOMPARE(role(model, 0, TaskbarModel::IconRole).toString(), QStringLiteral("mail"));
        QVERIFY(role(model, 0, TaskbarModel::PinnedRole).toBool());
        QVERIFY(role(model, 0, TaskbarModel::HasWindowsRole).toBool());
        QVERIFY(!role(model, 1, TaskbarModel::HasWindowsRole).toBool());
        QCOMPARE(role(model, 1, TaskbarModel::ActiveWindowIdRole).toString(), QString());
        QCOMPARE(role(model, 2, TaskbarModel::DesktopIdRole).toString(), QString());
        QCOMPARE(role(model, 2, TaskbarModel::KeyRole).toString(), QStringLiteral("app:term"));
        QCOMPARE(role(model, 3, TaskbarModel::DesktopIdRole).toString(), QStringLiteral("player.desktop"));
        QCOMPARE(model.windowState(0),
            QVariantMap({ { QStringLiteral("maximized"), true }, { QStringLiteral("minimized"), false } }));
        QVERIFY(model.windowState(1).isEmpty());
        QCOMPARE(model.roleNames().value(TaskbarModel::BadgeCountRole), QByteArray("badgeCount"));

        // Unpinned: the Flatpak, still pinned, takes the window.
        model.unpin(QStringLiteral("mail.desktop"));
        QCOMPARE(role(model, 0, TaskbarModel::DesktopIdRole).toString(), QStringLiteral("org.example.Mail.desktop"));
        QCOMPARE(role(model, 0, TaskbarModel::WindowCountRole).toInt(), 1);
        model.unpin(QStringLiteral("org.example.Mail.desktop"));
        QCOMPARE(names(model).first(), QStringLiteral("Editor"));
        QCOMPARE(names(model).last(), QStringLiteral("Mail"));
        QVERIFY(!role(model, int(names(model).size()) - 1, TaskbarModel::PinnedRole).toBool());
        model.pin(QStringLiteral("mail.desktop"));
        model.pin(QStringLiteral("mail.desktop"));
        QCOMPARE(model.pinnedIds().count(QStringLiteral("mail.desktop")), 1);

        // Closed: the dialog loses its parent, the pinned button stays.
        mail->closeNow();
        QVERIFY(!dialog->parentWindow);
        QCOMPARE(names(model).count(QStringLiteral("Mail")), 1);
        const int mailRow = int(names(model).indexOf(QStringLiteral("Mail")));
        QCOMPARE(role(model, mailRow, TaskbarModel::WindowIdsRole).toStringList(),
            QStringList { QStringLiteral("w-dialog") });
    }

    // Dragged within its group: pinned apps stay first, and the order lasts.
    void moveWithinTheGroup()
    {
        AppModel apps;
        apps.reload();
        FakeWindows windows;
        TaskbarModel model(&apps, &windows);
        model.setPinnedIds(
            { QStringLiteral("mail.desktop"), QStringLiteral("editor.desktop"), QStringLiteral("missing.desktop") });
        windows.open(QStringLiteral("w-a"), QStringLiteral("Alpha"), QStringLiteral("alpha"));
        windows.open(QStringLiteral("w-b"), QStringLiteral("Beta"), QStringLiteral("beta"));

        model.move(0, 1);
        QCOMPARE(model.pinnedIds(),
            QStringList({ QStringLiteral("editor.desktop"), QStringLiteral("mail.desktop"),
                QStringLiteral("missing.desktop") }));
        // A pinned app doesn't go among the others, nor them among the pinned.
        model.move(0, 3);
        QCOMPARE(names(model).mid(0, 2), QStringList({ QStringLiteral("Mail"), QStringLiteral("Editor") }));
        model.move(3, 0);
        QCOMPARE(names(model).mid(2), QStringList({ QStringLiteral("Beta"), QStringLiteral("Alpha") }));
        model.move(2, 2);
        model.move(-1, 2);
        model.move(2, 9);
        QCOMPARE(names(model).mid(2), QStringList({ QStringLiteral("Beta"), QStringLiteral("Alpha") }));

        // The order of the pins comes back with the next shell.
        TaskbarModel next(&apps, &windows);
        QCOMPARE(next.pinnedIds(), model.pinnedIds());
    }

    // A click: the app's window comes, goes or the next one comes.
    void clicksAndMenus()
    {
        AppModel apps;
        apps.reload();
        FakeWindows windows;
        TaskbarModel model(&apps, &windows);
        FakeWindow* first = windows.open(QStringLiteral("w-1"), QStringLiteral("One"), QStringLiteral("editor"));
        FakeWindow* second = windows.open(QStringLiteral("w-2"), QStringLiteral("Two"), QStringLiteral("editor"));
        windows.open(QStringLiteral("w-3"), QStringLiteral("Song"), QStringLiteral("player"), { activated });
        QCOMPARE(model.rowCount(), 2);

        // None in front: the most recent one.
        second->describe(QStringLiteral("Two"), QStringLiteral("editor"), { activated });
        second->describe(QStringLiteral("Two"), QStringLiteral("editor"));
        model.activate(0);
        QCOMPARE(requests.takeLast(), QStringLiteral("activate w-2"));
        // One of several in front: the next one.
        first->describe(QStringLiteral("One"), QStringLiteral("editor"), { activated });
        model.activate(0);
        QCOMPARE(requests.takeLast(), QStringLiteral("activate w-2"));
        // The only one in front: minimized.
        model.activate(1);
        QCOMPARE(requests.takeLast(), QStringLiteral("minimize w-3"));
        model.activate(7);
        QVERIFY(model.showApp(QStringLiteral("editor.desktop")));
        QCOMPARE(requests.takeLast(), QStringLiteral("activate w-1"));
        QVERIFY(!model.showApp(QStringLiteral("mail.desktop")));
        QVERIFY(!model.showApp(QString()));

        model.raiseWindow(1);
        QCOMPARE(requests.takeLast(), QStringLiteral("activate w-3"));
        model.windowAction(0, QStringLiteral("maximize"));
        QCOMPARE(requests.takeLast(), QStringLiteral("maximize w-1"));
        model.windowAction(0, QStringLiteral("minimize"));
        QCOMPARE(requests.takeLast(), QStringLiteral("minimize w-1"));
        model.windowAction(0, QStringLiteral("close"));
        QCOMPARE(requests.takeLast(), QStringLiteral("close w-1"));
        first->describe(QStringLiteral("One"), QStringLiteral("editor"), { activated, maximized });
        model.windowAction(0, QStringLiteral("restore"));
        QCOMPARE(requests.takeLast(), QStringLiteral("unmaximize w-1"));
        first->describe(QStringLiteral("One"), QStringLiteral("editor"), { minimized });
        model.windowAction(0, QStringLiteral("restore"));
        QCOMPARE(requests.takeLast(), QStringLiteral("activate w-1"));
        const qsizetype before = requests.size();
        model.windowAction(1, QStringLiteral("restore")); // neither minimized nor maximized
        QCOMPARE(requests.size(), before);
        model.windowAction(0, QStringLiteral("move"));
        QCOMPARE(commands.takeLast(), QByteArray("window active move"));
        QCOMPARE(requests.takeLast(), QStringLiteral("activate w-1"));
        model.windowAction(5, QStringLiteral("close"));

        requests.clear();
        model.closeWindows(0);
        QCOMPARE(requests, QStringList({ QStringLiteral("close w-1"), QStringLiteral("close w-2") }));
        model.endTask(1);
        QCOMPARE(commands.takeLast(), QByteArray("end-task player"));
        model.endTask(4);
        QVERIFY(commands.isEmpty());
        QCOMPARE(model.appIds(0), QStringList { QStringLiteral("editor") }); // once for both windows
        QVERIFY(model.appIds(9).isEmpty());
    }

    // "Show desktop": everything down, then the same ones back, the most
    // recent last.
    void showDesktop()
    {
        AppModel apps;
        apps.reload();
        FakeWindows windows;
        TaskbarModel model(&apps, &windows);
        FakeWindow* older = windows.open(QStringLiteral("w-old"), QStringLiteral("Old"), QStringLiteral("a"));
        older->describe(QStringLiteral("Old"), QStringLiteral("a"), { activated });
        FakeWindow* newer = windows.open(QStringLiteral("w-new"), QStringLiteral("New"), QStringLiteral("b"));
        newer->describe(QStringLiteral("New"), QStringLiteral("b"), { activated });
        windows.open(QStringLiteral("w-down"), QStringLiteral("Down"), QStringLiteral("c"), { minimized });

        model.toggleDesktop();
        QCOMPARE(requests, QStringList({ QStringLiteral("minimize w-old"), QStringLiteral("minimize w-new") }));
        requests.clear();
        older->describe(QStringLiteral("Old"), QStringLiteral("a"), { minimized });
        newer->describe(QStringLiteral("New"), QStringLiteral("b"), { minimized });
        model.toggleDesktop();
        QCOMPARE(requests, QStringList({ QStringLiteral("activate w-old"), QStringLiteral("activate w-new") }));
    }

    // One taskbar per display: its desktop's and display's windows, the
    // badges and the attention its settings let through.
    void perDisplayTaskbar()
    {
        AppModel apps;
        apps.reload();
        FakeWindows windows;
        TaskbarModel source(&apps, &windows);
        source.setPinnedIds({ QStringLiteral("mail.desktop"), QStringLiteral("editor.desktop") });
        Config config;
        Workspaces workspaces;
        LauncherEntry badges;
        TaskbarModel model(&source, &config, &workspaces, &badges, QStringLiteral("DP-1"), nullptr);

        FakeWindow* mail = windows.open(QStringLiteral("w-mail"), QStringLiteral("Inbox"), QStringLiteral("mail"));
        FakeWindow* dialog = windows.open(QStringLiteral("w-ask"), QStringLiteral("Ask"), QStringLiteral("mail"));
        dialog->parentWindow = mail;
        emit dialog->changed();
        windows.open(QStringLiteral("w-far"), QStringLiteral("Far"), QStringLiteral("far"));
        windows.open(QStringLiteral("w-side"), QStringLiteral("Side"), QStringLiteral("side"));
        const auto state = [&](const QVariantMap& extra) {
            QVariantMap json { { "current", 0 }, { "names", QStringList { "1", "2" } },
                { "windows", QVariantMap { { "w-mail", 0 }, { "w-ask", 0 }, { "w-far", 1 }, { "w-side", 0 } } },
                { "outputs", QVariantMap { { "w-mail", "DP-1" }, { "w-far", "DP-1" }, { "w-side", "HDMI-1" } } } };
            json.insert(extra);
            workspaces.update(QJsonDocument::fromVariant(json).toJson());
        };
        state({});
        // Another desktop's window is left out; another display's stays.
        QCOMPARE(
            names(model), QStringList({ QStringLiteral("Mail"), QStringLiteral("Editor"), QStringLiteral("Side") }));

        // Moved in the per-display model: moved in the shared one.
        model.move(0, 1);
        QCOMPARE(source.pinnedIds().first(), QStringLiteral("editor.desktop"));
        model.move(1, 0);

        // Only this display's windows, all desktops.
        QVERIFY(writeConf("appsOn=window\nwindowsFrom=all\n"));
        QTRY_COMPARE(config.taskbarWindowsFrom(), QStringLiteral("all"));
        QCOMPARE(
            names(model), QStringList({ QStringLiteral("Mail"), QStringLiteral("Editor"), QStringLiteral("Far") }));

        // A dialog asking for attention: its owner's button, and the click
        // brings the dialog.
        state({ { "attention", QStringList { "w-ask" } } });
        QVERIFY(role(model, 0, TaskbarModel::AttentionRole).toBool());
        model.activate(0);
        QCOMPARE(requests.takeLast(), QStringLiteral("activate w-ask"));

        // Badges and progress, from the app.
        const QString name = QStringLiteral("taskbar-model-badges");
        {
            const auto bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, name);
            QVERIFY(bus.isConnected());
            QSignalSpy changes(&badges, &LauncherEntry::changed);
            publishBadge(bus, QStringLiteral("mail.desktop"),
                { { "count", QVariant::fromValue<qlonglong>(120) }, { "progress", 0.5 },
                    { "progress-visible", true } });
            publishBadge(bus, QStringLiteral("editor.desktop"),
                { { "count", QVariant::fromValue<qlonglong>(7) }, { "urgent", true } });
            QTRY_COMPARE(changes.count(), 2);
        }
        QCOMPARE(role(model, 0, TaskbarModel::BadgeCountRole).toString(), QStringLiteral("99+"));
        QCOMPARE(role(model, 0, TaskbarModel::ProgressRole).toDouble(), 0.5);
        QCOMPARE(role(model, 1, TaskbarModel::BadgeCountRole).toString(), QStringLiteral("7"));
        QCOMPARE(role(model, 2, TaskbarModel::BadgeCountRole).toString(), QString());
        // Urgent with no window: the pinned button asks for attention.
        QVERIFY(role(model, 1, TaskbarModel::AttentionRole).toBool());

        // Settings turn them off.
        QVERIFY(writeConf("appsOn=window\nwindowsFrom=all\nbadges=false\nprogress=false\nattention=false\n"));
        QTRY_VERIFY(!config.taskbarBadges());
        QCOMPARE(role(model, 0, TaskbarModel::BadgeCountRole).toString(), QString());
        QCOMPARE(role(model, 0, TaskbarModel::ProgressRole).toDouble(), -1.0);
        QVERIFY(!role(model, 0, TaskbarModel::AttentionRole).toBool());
        QVERIFY(!role(model, 1, TaskbarModel::AttentionRole).toBool());

        // A window only on another desktop: the button stays, the click
        // brings it, End task reaches it.
        QVERIFY(writeConf("appsOn=window\n"));
        QTRY_COMPARE(config.taskbarWindowsFrom(), QStringLiteral("desktop"));
        state({ { "current", 1 } });
        QCOMPARE(role(model, 0, TaskbarModel::WindowCountRole).toInt(), 0);
        QVERIFY(role(model, 0, TaskbarModel::HasWindowsRole).toBool());
        model.activate(0);
        QCOMPARE(requests.takeLast(), QStringLiteral("activate w-mail"));
        model.endTask(0);
        QCOMPARE(commands.takeLast(), QByteArray("end-task mail"));
        model.toggleDesktop();
        QVERIFY(requests.contains(QStringLiteral("minimize w-mail")));
        QDBusConnection::disconnectFromBus(name);
    }

    // A launcher dropped on the taskbar: pinned before the button it fell on.
    void dropALauncher()
    {
        AppModel apps;
        apps.reload();
        FakeWindows windows;
        TaskbarModel model(&apps, &windows);
        model.setPinnedIds({ QStringLiteral("mail.desktop"), QStringLiteral("editor.desktop") });

        const QUrl launcher = QUrl::fromLocalFile(home().filePath(QStringLiteral("launchers/backup.desktop")));
        QVERIFY(model.isLauncherDrop(launcher));
        QVERIFY(!model.isLauncherDrop(QUrl(QStringLiteral("https://example.com/a.desktop"))));
        QVERIFY(!model.pinDropped(QUrl::fromLocalFile(home().filePath(QStringLiteral("none.desktop"))), 0));

        QVERIFY(model.pinDropped(launcher, 1));
        const QString imported = model.pinnedIds().at(1);
        QVERIFY(AppModel::isImportedLauncher(imported));
        // Same name as Mail, its own button all the same.
        QCOMPARE(
            names(model), QStringList({ QStringLiteral("Mail"), QStringLiteral("Mail"), QStringLiteral("Editor") }));
        // Dropped again at the end: moved there; on itself: nothing changes.
        QVERIFY(model.pinDropped(launcher, -1));
        QCOMPARE(model.pinnedIds().last(), imported);
        QVERIFY(model.pinDropped(launcher, 2));
        QCOMPARE(model.pinnedIds().last(), imported);
        // An installed app's file is that app.
        QVERIFY(model.pinDropped(
            QUrl::fromLocalFile(home().filePath(QStringLiteral("data/applications/player.desktop"))), 0));
        QCOMPARE(model.pinnedIds().first(), QStringLiteral("player.desktop"));
    }
};

int main(int argc, char** argv)
{
    if (!isolate()) {
        return 1;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // No portal started on the private bus: it would outlive the test.
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("vela-shell"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    TaskbarModelTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "taskbarmodel_test.moc"
