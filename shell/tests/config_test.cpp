// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The shell's Config (src/config.h) on a private vela-shell.conf: what it
// reads before anything is chosen, and what it tells QML and C++ when
// Settings (or the desktop's menu) rewrites the file, as QSaveFile does:
// the clock's date, format and seconds (src/clockformat.h), the number of
// notifications, Start's pins and icon (src/starticon.h), the desktop icons.

#include "clockformat.h"
#include "config.h"
#include "privatehome.h"
#include "../../common/taskbarsettings.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QSaveFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

using namespace vela::test;

class ConfigTest : public QObject {
    Q_OBJECT


    // As QSettings writes it from another process: a new file in its place.
    static void write(const QByteArray& text)
    {
        QSaveFile file(confPath());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(text);
        QVERIFY(file.commit());
    }

private slots:
    void init()
    {
        QFile::remove(confPath());
        QVERIFY(QDir().mkpath(QFileInfo(confPath()).absolutePath()));
    }

    void aNewProfileStillFollowsSettings()
    {
        // No ~/.config/Vela when the shell starts: Settings makes it with
        // its first write, which the shell must see.
        QDir(QFileInfo(confPath()).absolutePath()).removeRecursively();
        Config config;
        QSignalSpy clock(&config, &Config::clockChanged);
        QDir().mkpath(QFileInfo(confPath()).absolutePath());
        write("[clock]\nhours=12\n");
        QVERIFY(clock.wait(3000));
        QCOMPARE(config.timeFormat(), QStringLiteral("h:mm AP"));
    }

    void defaultsBeforeAnyChoice()
    {
        Config config;
        QVERIFY(config.clockDate());
        QVERIFY(!config.clockSeconds());
        QVERIFY(config.notificationCount());
        QCOMPARE(config.timeFormat(), QStringLiteral("HH:mm"));
        QCOMPARE(config.clockFormat(), QStringLiteral("HH:mm"));
        QVERIFY(config.startPinned());
        QCOMPARE(config.startIconSource(), QString());
        QVERIFY(config.showDesktopIcons());
        QVERIFY(config.silenceWhileSharing()); // on unless turned off
    }

    // Settings' switch, live (docs/screen-sharing.md §7).
    void followsSilenceWhileSharing()
    {
        Config config;
        QSignalSpy silence(&config, &Config::silenceWhileSharingChanged);
        write("[notifications]\nsilenceWhileSharing=false\n");
        QVERIFY(silence.wait(3000));
        QVERIFY(!config.silenceWhileSharing());
        QCOMPARE(silence.last().first().toBool(), false);
        write("[notifications]\nsilenceWhileSharing=true\n");
        QVERIFY(silence.wait(3000));
        QVERIFY(config.silenceWhileSharing());
    }

    // The battery notifications' settings, live, and a bad level ignored
    // (docs/battery-notifications.md §6).
    void followsTheBatterySettings()
    {
        Config config;
        QVERIFY(config.batteryNotify());
        QCOMPARE(config.batteryLowLevel(), 20);
        QVERIFY(config.deviceBatteryNotify());
        QSignalSpy battery(&config, &Config::batteryChanged);
        write("[battery]\nnotify=false\nlowLevel=35\ndevices=false\n");
        QVERIFY(battery.wait(3000));
        QVERIFY(!config.batteryNotify());
        QCOMPARE(config.batteryLowLevel(), 35);
        QVERIFY(!config.deviceBatteryNotify());
        write("[battery]\nlowLevel=200\n");
        QVERIFY(battery.wait(3000));
        QCOMPARE(config.batteryLowLevel(), 20);
        QVERIFY(config.batteryNotify());
        write("[battery]\nlowLevel=lots\n");
        QTest::qWait(500);
        QCOMPARE(config.batteryLowLevel(), 20);
    }

    void followsTheClocksChoices()
    {
        Config config;
        QSignalSpy clock(&config, &Config::clockChanged);
        write("[clock]\nhours=12\n[taskbar]\nclockSeconds=true\nshowDate=false\nnotificationCount=false\n");
        QVERIFY(clock.wait(3000));
        QVERIFY(!config.clockDate());
        QVERIFY(config.clockSeconds());
        QVERIFY(!config.notificationCount());
        QCOMPARE(config.timeFormat(), QStringLiteral("h:mm AP"));
        QCOMPARE(config.clockFormat(), QStringLiteral("h:mm:ss AP"));
        // 24-hour again, seconds kept.
        write("[clock]\nhours=24\n[taskbar]\nclockSeconds=true\n");
        QVERIFY(clock.wait(3000));
        QCOMPARE(config.clockFormat(), QStringLiteral("HH:mm:ss"));
        QVERIFY(config.clockDate());
        QVERIFY(config.notificationCount());
        // Only the count.
        write("[clock]\nhours=24\n[taskbar]\nclockSeconds=true\nnotificationCount=false\n");
        QVERIFY(clock.wait(3000));
        QVERIFY(!config.notificationCount());
        QCOMPARE(config.clockFormat(), QStringLiteral("HH:mm:ss"));
        // Only the seconds.
        write("[clock]\nhours=24\n[taskbar]\nclockSeconds=false\nnotificationCount=false\n");
        QVERIFY(clock.wait(3000));
        QCOMPARE(config.clockFormat(), QStringLiteral("HH:mm"));
    }

    void unrelatedChangesStayQuiet()
    {
        write("[clock]\nhours=12\n");
        Config config;
        QSignalSpy clock(&config, &Config::clockChanged);
        QSignalSpy start(&config, &Config::startChanged);
        QSignalSpy desktop(&config, &Config::desktopIconsChanged);
        QSignalSpy taskbar(&config, &Config::taskbarChanged);
        write("[clock]\nhours=12\n[taskbar]\nalignment=left\n");
        QVERIFY(taskbar.wait(3000));
        QCOMPARE(clock.count(), 0);
        QCOMPARE(start.count(), 0);
        QCOMPARE(desktop.count(), 0);
    }

    void taskbarDefaults()
    {
        Config config;
        QCOMPARE(config.taskbarAlignment(), QStringLiteral("center"));
        QCOMPARE(config.taskbarPosition(), QStringLiteral("bottom"));
        QCOMPARE(config.taskbarWindowsFrom(), QStringLiteral("desktop"));
        QCOMPARE(config.taskbarAppsOn(), QStringLiteral("all"));
        QCOMPARE(config.taskbarClickActive(), QStringLiteral("previews"));
        QCOMPARE(config.taskbarTrayShown(), QStringList());
        QVERIFY(config.taskbarAttention());
        QVERIFY(config.taskbarBadges());
        QVERIFY(config.taskbarProgress());
        QVERIFY(config.taskbarNumberHints());
        QVERIFY(config.taskbarAllScreens());
        QVERIFY(config.endTask());
        QVERIFY(config.taskView());
        QVERIFY(config.showDesktopCorner());
    }

    // Every choice of Settings' taskbar page, live (docs/taskbar.md).
    void followsTheTaskbarChoices()
    {
        Config config;
        QSignalSpy taskbar(&config, &Config::taskbarChanged);
        write("[taskbar]\nalignment=left\nposition=\" Left \"\nwindowsFrom=all\nappsOn=window\nclickActive=next\n"
              "trayShown=steam, \" discord \", , steam\nattention=false\nbadges=0\nprogress=FALSE\n"
              "numberHints=no\nallScreens=false\nendTask=false\ntaskView=false\nshowDesktopCorner=false\n");
        QVERIFY(taskbar.wait(3000));
        QCOMPARE(config.taskbarAlignment(), QStringLiteral("left"));
        QCOMPARE(config.taskbarPosition(), QStringLiteral("left"));
        QCOMPARE(config.taskbarWindowsFrom(), QStringLiteral("all"));
        QCOMPARE(config.taskbarAppsOn(), QStringLiteral("window"));
        QCOMPARE(config.taskbarClickActive(), QStringLiteral("next"));
        // Trimmed, without blanks or repeats, in the order chosen.
        QCOMPARE(config.taskbarTrayShown(), (QStringList { QStringLiteral("steam"), QStringLiteral("discord") }));
        QVERIFY(!config.taskbarAttention());
        QVERIFY(!config.taskbarBadges());
        QVERIFY(!config.taskbarProgress());
        // Only "false" and "0" turn a switch off.
        QVERIFY(config.taskbarNumberHints());
        QVERIFY(!config.taskbarAllScreens());
        QVERIFY(!config.endTask());
        QVERIFY(!config.taskView());
        QVERIFY(!config.showDesktopCorner());

        // Each edge, and anything else is the bottom.
        for (const char* edge : { "top", "right", "RIGHT", "middle", "" }) {
            taskbar.clear();
            write(QByteArray("[taskbar]\nposition=") + edge + "\n");
            const QString expected = QString::fromLatin1(edge).toLower();
            const QString position = expected == QLatin1String("top") || expected == QLatin1String("right")
                ? expected
                : QStringLiteral("bottom");
            QTRY_COMPARE_WITH_TIMEOUT(config.taskbarPosition(), position, 3000);
        }
        QCOMPARE(config.taskbarAlignment(), QStringLiteral("center"));
        QCOMPARE(config.taskbarTrayShown(), QStringList());
    }

    // The drawer's moves (docs/taskbar.md §8.2): saved at once, cleaned up.
    void savesTheTrayIconsShown()
    {
        Config config;
        QSignalSpy taskbar(&config, &Config::taskbarChanged);
        QVERIFY(config.setTaskbarTrayShown(
            { QStringLiteral(" steam "), QString(), QStringLiteral("discord"), QStringLiteral("steam") }));
        QCOMPARE(taskbar.count(), 1); // without waiting for the file watcher
        const QStringList shown { QStringLiteral("steam"), QStringLiteral("discord") };
        QCOMPARE(config.taskbarTrayShown(), shown);
        QSettings settings(confPath(), QSettings::IniFormat);
        QCOMPARE(settings.value(QStringLiteral("taskbar/trayShown")).toStringList(), shown);

        // The same list again: nothing to write or tell.
        QVERIFY(config.setTaskbarTrayShown(shown));
        QCOMPARE(taskbar.count(), 1);
        // Its own write read back changes nothing either.
        QTest::qWait(500);
        QCOMPARE(config.taskbarTrayShown(), shown);
        QCOMPARE(taskbar.count(), 1);

        QVERIFY(config.setTaskbarTrayShown({}));
        QCOMPARE(config.taskbarTrayShown(), QStringList());
        QCOMPARE(taskbar.count(), 2);
    }

    void refusesTheTrayIconsWhenTheFileCannotBeWritten()
    {
        if (geteuid() == 0) {
            QSKIP("root writes into a read-only directory");
        }
        Config config;
        QSignalSpy taskbar(&config, &Config::taskbarChanged);
        const QString directory = QFileInfo(confPath()).absolutePath();
        QVERIFY(QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const bool saved = config.setTaskbarTrayShown({ QStringLiteral("steam") });
        QVERIFY(
            QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QVERIFY(!saved);
        QCOMPARE(config.taskbarTrayShown(), QStringList());
        QCOMPARE(taskbar.count(), 0);
    }

    void readsTheTaskbarPosition()
    {
        QSettings settings(home().filePath(QStringLiteral("position.conf")), QSettings::IniFormat);
        QCOMPARE(vela::taskbar::position(settings), QStringLiteral("bottom"));
        settings.setValue(QStringLiteral("taskbar/position"), QStringLiteral(" Top"));
        QCOMPARE(vela::taskbar::position(settings), QStringLiteral("top"));
        settings.setValue(QStringLiteral("taskbar/position"), QStringLiteral("left"));
        QCOMPARE(vela::taskbar::position(settings), QStringLiteral("left"));
        settings.setValue(QStringLiteral("taskbar/position"), QStringLiteral("bottom-left"));
        QCOMPARE(vela::taskbar::position(settings), QStringLiteral("bottom"));
        QVERIFY(vela::taskbar::enabled(settings, QStringLiteral("taskbar/missing")));
        settings.setValue(QStringLiteral("taskbar/badges"), QStringLiteral(" False "));
        QVERIFY(!vela::taskbar::enabled(settings, QStringLiteral("taskbar/badges")));
        settings.setValue(QStringLiteral("taskbar/badges"), 0);
        QVERIFY(!vela::taskbar::enabled(settings, QStringLiteral("taskbar/badges")));
        settings.setValue(QStringLiteral("taskbar/badges"), true);
        QVERIFY(vela::taskbar::enabled(settings, QStringLiteral("taskbar/badges")));
    }

    void followsStart()
    {
        Config config;
        QSignalSpy start(&config, &Config::startChanged);
        write("[start]\nshowPinned=false\n");
        QVERIFY(start.wait(3000));
        QVERIFY(!config.startPinned());

        // A picture there is: drawn; gone: the sail.
        const QString picture = home().filePath(QStringLiteral("start.png"));
        QVERIFY(paint(picture, Qt::red));
        write("[start]\nicon=picture\niconPicture=" + picture.toUtf8() + "\n");
        QVERIFY(start.wait(3000));
        QCOMPARE(config.startIconSource(), picture);
        QVERIFY(config.startPinned());
        QFile::remove(picture);
        write("[start]\nicon=picture\niconPicture=" + picture.toUtf8() + "\nshowPinned=true\n");
        QVERIFY(start.wait(3000));
        QCOMPARE(config.startIconSource(), QString());

        // The distribution's logo (VELA_OS_RELEASE, main()).
        write("[start]\nicon=distribution\n");
        QVERIFY(start.wait(3000));
        QCOMPARE(config.startIconSource(), home().filePath(QStringLiteral("data/icons/test-logo.svg")));
    }

    void findsTheLogoOnceTheIconThemeIsSet()
    {
        // Vela.Controls sets the icon theme after Config starts (main.cpp):
        // a logo only there shows once refreshStartIcon() looks again.
        const QByteArray release = qgetenv("VELA_OS_RELEASE");
        qputenv("VELA_OS_RELEASE", themedOsRelease().toUtf8());
        QIcon::setThemeSearchPaths({});
        write("[start]\nicon=distribution\n");
        Config config;
        QCOMPARE(config.startIconSource(), QString());
        QSignalSpy start(&config, &Config::startChanged);
        QIcon::setThemeSearchPaths({ home().filePath(QStringLiteral("themes")) });
        QIcon::setThemeName(QStringLiteral("test"));
        config.refreshStartIcon();
        QCOMPARE(start.count(), 1);
        QCOMPARE(config.startIconSource(), QStringLiteral("themed-logo"));
        config.refreshStartIcon(); // nothing new: quiet
        QCOMPARE(start.count(), 1);
        QIcon::setThemeSearchPaths({});
        qputenv("VELA_OS_RELEASE", release);
    }

    void watchesThePictureChosen()
    {
        // The picture changes or goes without Settings writing a thing.
        const QString picture = home().filePath(QStringLiteral("watched.png"));
        QVERIFY(paint(picture, Qt::red));
        write("[start]\nicon=picture\niconPicture=" + picture.toUtf8() + "\n");
        Config config;
        QCOMPARE(config.startIconSource(), picture);
        QSignalSpy start(&config, &Config::startChanged);

        // Painted over in place (an editor): the same name, a new image.
        const int revision = config.startIconRevision();
        QTest::qWait(50); // a later modification time
        QVERIFY(paint(picture, Qt::blue));
        QVERIFY(start.wait(3000));
        QCOMPARE(config.startIconSource(), picture);
        QVERIFY(config.startIconRevision() > revision);

        // Replaced by an editor's new file: still watched.
        QVERIFY(paint(picture + QStringLiteral(".new"), Qt::green));
        QVERIFY(QFile::remove(picture));
        QVERIFY(QFile::rename(picture + QStringLiteral(".new"), picture));
        QVERIFY(start.wait(3000));
        QCOMPARE(config.startIconSource(), picture);

        // Deleted: the sail.
        QVERIFY(QFile::remove(picture));
        QTRY_COMPARE_WITH_TIMEOUT(config.startIconSource(), QString(), 3000);
    }

    void tellsTheDesktopAboutItsIcons()
    {
        Config config;
        QSignalSpy desktop(&config, &Config::desktopIconsChanged);
        write("[desktop]\nshowIcons=false\n");
        QVERIFY(desktop.wait(3000));
        QCOMPARE(desktop.takeFirst().at(0).toBool(), false);
        QVERIFY(!config.showDesktopIcons());
        write("[desktop]\nshowIcons=true\n");
        QVERIFY(desktop.wait(3000));
        QCOMPARE(desktop.takeFirst().at(0).toBool(), true);
    }

    void clockFormats()
    {
        QCOMPARE(vela::clock::timeFormat(true), QStringLiteral("HH:mm"));
        QCOMPARE(vela::clock::timeFormat(true, true), QStringLiteral("HH:mm:ss"));
        QCOMPARE(vela::clock::timeFormat(false), QStringLiteral("h:mm AP"));
        QCOMPARE(vela::clock::timeFormat(false, true), QStringLiteral("h:mm:ss AP"));
        // Anything but 12 is 24-hour.
        QSettings settings(home().filePath(QStringLiteral("hours.conf")), QSettings::IniFormat);
        QVERIFY(vela::clock::hour24(settings));
        settings.setValue(QStringLiteral("clock/hours"), 13);
        QVERIFY(vela::clock::hour24(settings));
        settings.setValue(QStringLiteral("clock/hours"), 12);
        QVERIFY(!vela::clock::hour24(settings));
    }
};

int main(int argc, char** argv)
{
    // A distribution whose logo is a loose file, as CachyOS's.
    if (!isolate() || !writeFile(QStringLiteral("os-release"), "NAME=Test\nLOGO=test-logo\n")
        || !writeFile(QStringLiteral("data/icons/test-logo.svg"), "<svg xmlns=\"http://www.w3.org/2000/svg\"/>")) {
        return 1;
    }

    QGuiApplication app(argc, argv); // QIcon, as starticon.h asks
    QGuiApplication::setApplicationName(QStringLiteral("vela-shell"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    ConfigTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "config_test.moc"
