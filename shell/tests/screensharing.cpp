// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Who shares the screen, as the shell names it (src/screensharing.h,
// docs/screen-sharing.md §5, §6): the compositor's `watch capture` lines to
// apps and what they capture, the portal's apps from PipeWire, the second of
// grace, and a fake compositor that restarts.

#include "screensharing.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

QJsonObject output(const QString& name, const QString& make = {}, const QString& model = {},
    const QString& description = {})
{
    return { { QStringLiteral("type"), QStringLiteral("output") }, { QStringLiteral("name"), name },
        { QStringLiteral("make"), make }, { QStringLiteral("model"), model },
        { QStringLiteral("description"), description } };
}

QJsonObject window(const QString& appId, const QString& title = QStringLiteral("Title"))
{
    return { { QStringLiteral("type"), QStringLiteral("window") }, { QStringLiteral("id"), QStringLiteral("w1") },
        { QStringLiteral("app_id"), appId }, { QStringLiteral("title"), title } };
}

QJsonObject client(const QString& binary, const QJsonArray& targets, const QString& appId = {},
    const QString& windowId = {}, bool portal = false)
{
    return { { QStringLiteral("pid"), 42 }, { QStringLiteral("binary"), binary }, { QStringLiteral("app_id"), appId },
        { QStringLiteral("window"), windowId }, { QStringLiteral("portal"), portal },
        { QStringLiteral("targets"), targets } };
}

QJsonObject state(const QJsonArray& clients)
{
    return { { QStringLiteral("clients"), clients } };
}

QByteArray line(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

// Start's names: OBS and Konsole are installed, wf-recorder isn't.
ScreenSharing::App resolve(const QStringList& candidates)
{
    for (const QString& c : candidates) {
        if (c == QLatin1String("com.obsproject.Studio") || c == QLatin1String("obs")) {
            return { QStringLiteral("com.obsproject.Studio.desktop"), QStringLiteral("OBS Studio"),
                QStringLiteral("com.obsproject.Studio") };
        }
        if (c == QLatin1String("org.kde.konsole")) {
            return { QStringLiteral("org.kde.konsole.desktop"), QStringLiteral("Konsole"),
                QStringLiteral("utilities-terminal") };
        }
    }
    return {};
}

QVariantMap firefox()
{
    return { { QStringLiteral("name"), QStringLiteral("Firefox") }, { QStringLiteral("icon"), QStringLiteral("firefox") },
        { QStringLiteral("desktopId"), QStringLiteral("firefox.desktop") } };
}

QVariantMap user(const QVariantList& users, int index)
{
    return users.value(index).toMap();
}

} // namespace

class ScreenSharingTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void namesAnAppByItsWindowOrBinary()
    {
        const QVariantList users = ScreenSharing::describe(
            state({ client(QStringLiteral("obs"), { output(QStringLiteral("DP-3")) },
                        QStringLiteral("com.obsproject.Studio"), QStringLiteral("w9")),
                client(QStringLiteral("wf-recorder"), { output(QStringLiteral("DP-3")) }) }),
            resolve, {}, 1);
        QCOMPARE(users.size(), 2);
        QCOMPARE(user(users, 0).value("name").toString(), QStringLiteral("OBS Studio"));
        QCOMPARE(user(users, 0).value("icon").toString(), QStringLiteral("com.obsproject.Studio"));
        QCOMPARE(user(users, 0).value("desktopId").toString(), QStringLiteral("com.obsproject.Studio.desktop"));
        QCOMPARE(user(users, 0).value("window").toString(), QStringLiteral("w9")); // to bring it forward
        QCOMPARE(user(users, 0).value("what").toString(), QStringLiteral("Your screen"));
        // Not installed: its binary, and a generic icon.
        QCOMPARE(user(users, 1).value("name").toString(), QStringLiteral("wf-recorder"));
        QCOMPARE(user(users, 1).value("icon").toString(), QStringLiteral("video-display"));
        QCOMPARE(user(users, 1).value("desktopId").toString(), QString());
    }

    void namesWhatIsCaptured()
    {
        const QJsonArray targets { output(QStringLiteral("DP-3"), QStringLiteral("Dell"), QStringLiteral("U2720Q")),
            output(QStringLiteral("HDMI-A-1"), {}, {}, QStringLiteral("LG FULL HD")),
            output(QStringLiteral("HEADLESS-1")), window(QStringLiteral("org.kde.konsole")),
            window(QStringLiteral("org.unknown.App")), window(QString()) };
        const QVariantMap two = user(ScreenSharing::describe(state({ client(QStringLiteral("obs"), targets) }),
                                         resolve, {}, 2),
            0);
        QCOMPARE(two.value("targets").toStringList(),
            QStringList({ QStringLiteral("Dell U2720Q"), QStringLiteral("LG FULL HD"), QStringLiteral("HEADLESS-1"),
                QStringLiteral("A window of Konsole"), QStringLiteral("A window of org.unknown.App"),
                QStringLiteral("A window") }));
        QVERIFY(two.value("screen").toBool());
        // With one output there is only one screen to name.
        const QVariantMap one = user(ScreenSharing::describe(
                                         state({ client(QStringLiteral("obs"), { output(QStringLiteral("DP-3"),
                                             QStringLiteral("Dell"), QStringLiteral("U2720Q")) }) }),
                                         resolve, {}, 1),
            0);
        QCOMPARE(one.value("what").toString(), QStringLiteral("Your screen"));
    }

    void thePortalsAppsComeFromPipeWire()
    {
        const QJsonObject portal = state({ client(QStringLiteral("xdg-desktop-portal-wlr"),
            { window(QStringLiteral("org.kde.konsole")) }, {}, {}, true) });
        // Not linked yet (or no PipeWire): an app.
        QVariantList users = ScreenSharing::describe(portal, resolve, {}, 1);
        QCOMPARE(users.size(), 1);
        QCOMPARE(user(users, 0).value("name").toString(), QStringLiteral("An app"));
        QCOMPARE(user(users, 0).value("what").toString(), QStringLiteral("A window of Konsole"));
        QVERIFY(!user(users, 0).value("screen").toBool());
        // Linked: each app, with what the portal shares.
        users = ScreenSharing::describe(portal, resolve,
            { firefox(), QVariantMap { { QStringLiteral("name"), QStringLiteral("Discord") } } }, 1);
        QCOMPARE(users.size(), 2);
        QCOMPARE(user(users, 0).value("name").toString(), QStringLiteral("Firefox"));
        QCOMPARE(user(users, 0).value("desktopId").toString(), QStringLiteral("firefox.desktop"));
        QCOMPARE(user(users, 0).value("what").toString(), QStringLiteral("A window of Konsole"));
        QCOMPARE(user(users, 1).value("name").toString(), QStringLiteral("Discord"));
        QCOMPARE(user(users, 1).value("icon").toString(), QStringLiteral("video-display"));
    }

    void oneEntryPerApp()
    {
        // OBS through the portal and directly at once.
        const QVariantList users = ScreenSharing::describe(
            state({ client(QStringLiteral("xdg-desktop-portal-wlr"), { window(QStringLiteral("org.kde.konsole")) }, {},
                        {}, true),
                client(QStringLiteral("obs"), { output(QStringLiteral("DP-3")) }, QStringLiteral("com.obsproject.Studio"),
                    QStringLiteral("w9")) }),
            resolve,
            { QVariantMap { { QStringLiteral("name"), QStringLiteral("OBS Studio") },
                { QStringLiteral("desktopId"), QStringLiteral("com.obsproject.Studio.desktop") } } },
            1);
        QCOMPARE(users.size(), 1);
        QCOMPARE(user(users, 0).value("what").toString(), QStringLiteral("A window of Konsole, Your screen"));
        QVERIFY(user(users, 0).value("screen").toBool());
        QCOMPARE(user(users, 0).value("window").toString(), QStringLiteral("w9"));
    }

    void namesWhatTheCompositorLeavesOpen()
    {
        // Nothing to name it by: "An app". Two unknown clients with the same
        // binary: one entry, the window of the second kept. No targets: none.
        const QVariantList users = ScreenSharing::describe(
            state({ client(QString(), { output(QStringLiteral("DP-3")) }),
                client(QStringLiteral("recorder"), { output(QStringLiteral("DP-3")) }),
                client(QStringLiteral("recorder"), { window(QStringLiteral("org.kde.konsole")) }, {},
                    QStringLiteral("w2")),
                client(QStringLiteral("idle"), {}) }),
            resolve, {}, 1);
        QCOMPARE(users.size(), 2);
        QCOMPARE(user(users, 0).value("name").toString(), QStringLiteral("An app"));
        QCOMPARE(user(users, 1).value("name").toString(), QStringLiteral("recorder"));
        QCOMPARE(user(users, 1).value("what").toString(), QStringLiteral("Your screen, A window of Konsole"));
        QCOMPARE(user(users, 1).value("window").toString(), QStringLiteral("w2"));
        QVERIFY(user(users, 1).value("screen").toBool());
    }

    void summary()
    {
        QCOMPARE(ScreenSharing::summarize({}), QString());
        QVariantMap a { { QStringLiteral("name"), QStringLiteral("Firefox") }, { QStringLiteral("screen"), true } };
        QCOMPARE(ScreenSharing::summarize({ a }), QStringLiteral("Firefox is sharing your screen"));
        a[QStringLiteral("screen")] = false;
        QCOMPARE(ScreenSharing::summarize({ a }), QStringLiteral("Firefox is sharing a window"));
        const QVariantMap b { { QStringLiteral("name"), QStringLiteral("OBS Studio") }, { QStringLiteral("screen"), true } };
        QCOMPARE(ScreenSharing::summarize({ a, b }),
            QStringLiteral("These apps are sharing your screen:\nFirefox\nOBS Studio"));
    }

    // The model against a fake compositor: shows at once, stays a second,
    // starts over with a restarted compositor.
    void followsTheCompositor()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("vela.sock"));
        auto server = std::make_unique<QLocalServer>();
        QVERIFY(server->listen(path));
        ScreenSharing sharing(resolve, nullptr, path, 300);
        QSignalSpy changed(&sharing, &ScreenSharing::changed);

        QTRY_VERIFY(server->hasPendingConnections());
        QLocalSocket* compositor = server->nextPendingConnection();
        QTRY_VERIFY(compositor->bytesAvailable() || compositor->waitForReadyRead(100));
        QCOMPARE(compositor->readAll(), QByteArray("watch capture\n"));

        compositor->write(line(state({})));
        QTest::qWait(50);
        QVERIFY(!sharing.active());
        // Not a state: ignored. A line in two pieces: read once whole.
        compositor->write("not json\n{\"outputs\":[]}\n");
        const QByteArray whole = line(state({ client(QStringLiteral("obs"), { output(QStringLiteral("DP-3")) }) }));
        compositor->write(whole.left(10));
        QTest::qWait(50);
        QVERIFY(!sharing.active());
        compositor->write(whole.mid(10));
        QTRY_VERIFY(sharing.active());
        compositor->write(line(state({})));
        QTRY_VERIFY_WITH_TIMEOUT(!sharing.active(), 1000);

        compositor->write(line(state({ client(QStringLiteral("obs"), { output(QStringLiteral("DP-3")) }) })));
        QTRY_VERIFY(sharing.active());
        QCOMPARE(sharing.summary(), QStringLiteral("OBS Studio is sharing your screen"));

        // It ends: still shown for the grace, then gone.
        compositor->write(line(state({})));
        QTest::qWait(150);
        QVERIFY(sharing.active());
        QTRY_VERIFY_WITH_TIMEOUT(!sharing.active(), 1000);

        // Back within the grace: never hidden.
        compositor->write(line(state({ client(QStringLiteral("obs"), { output(QStringLiteral("DP-3")) }) })));
        QTRY_VERIFY(sharing.active());
        compositor->write(line(state({})));
        QTest::qWait(150);
        compositor->write(line(state({ client(QStringLiteral("obs"), { output(QStringLiteral("DP-3")) }) })));
        QTest::qWait(300);
        QVERIFY(sharing.active());

        // The compositor goes away: what it said goes too, after the grace.
        server.reset();
        QTRY_VERIFY_WITH_TIMEOUT(!sharing.active(), 1500);

        // A new one: the model watches again.
        server = std::make_unique<QLocalServer>();
        QVERIFY(server->listen(path));
        QTRY_VERIFY_WITH_TIMEOUT(server->hasPendingConnections(), 3000);
        compositor = server->nextPendingConnection();
        QTRY_VERIFY(compositor->bytesAvailable() || compositor->waitForReadyRead(100));
        QCOMPARE(compositor->readAll(), QByteArray("watch capture\n"));
        compositor->write(line(state({ client(QStringLiteral("obs"), { output(QStringLiteral("DP-3")) }) })));
        QTRY_VERIFY(sharing.active());
        QVERIFY(changed.count() > 0);
    }

    void portalAppsArriveLater()
    {
        QTemporaryDir dir;
        ScreenSharing sharing(resolve, nullptr, dir.filePath(QStringLiteral("none.sock")), 300);
        sharing.apply(line(state({ client(QStringLiteral("xdg-desktop-portal-wlr"), { output(QStringLiteral("DP-3")) },
            {}, {}, true) })));
        QCOMPARE(sharing.summary(), QStringLiteral("An app is sharing your screen"));
        sharing.setPortalApps({ firefox() });
        QCOMPARE(sharing.summary(), QStringLiteral("Firefox is sharing your screen"));
        QSignalSpy changed(&sharing, &ScreenSharing::changed);
        sharing.setPortalApps({ firefox() }); // the same: nothing new
        QCOMPARE(changed.count(), 0);
    }

    // The session's socket ($XDG_RUNTIME_DIR/vela-$WAYLAND_DISPLAY.sock),
    // tried again until the compositor is there.
    void findsTheSessionsCompositor()
    {
        QTemporaryDir runtime;
        qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        qputenv("WAYLAND_DISPLAY", "wayland-test");
        auto* sharing = new ScreenSharing(resolve, nullptr, {}, 300);
        QTest::qWait(200); // refused: nobody listens yet
        QLocalServer server;
        QVERIFY(server.listen(runtime.filePath(QStringLiteral("vela-wayland-test.sock"))));
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
        QLocalSocket* compositor = server.nextPendingConnection();
        QTRY_VERIFY(compositor->bytesAvailable() || compositor->waitForReadyRead(100));
        QCOMPARE(compositor->readAll(), QByteArray("watch capture\n"));
        // One output on the offscreen platform: "Your screen".
        compositor->write(line(state({ client(QStringLiteral("obs"), { output(QStringLiteral("DP-3"),
            QStringLiteral("Dell"), QStringLiteral("U2720Q")) }) })));
        QTRY_VERIFY(sharing->active());
        QCOMPARE(sharing->users().first().toMap().value("what").toString(), QStringLiteral("Your screen"));
        delete sharing;
    }
};

// A real QGuiApplication (offscreen): the screens are counted.
QTEST_MAIN(ScreenSharingTest)
#include "screensharing.moc"
