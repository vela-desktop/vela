// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The shell's Config (src/config.h) on a private vela-shell.conf: what it
// reads before anything is chosen, and what it tells QML and C++ when
// Settings (or the desktop's menu) rewrites the file, as QSaveFile does:
// the clock's date, format and seconds (src/clockformat.h), the number of
// notifications, Start's pins and icon (src/starticon.h), the desktop icons.

#include "clockformat.h"
#include "config.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

// Everything the test reads and writes, made on first use, gone at exit.
QTemporaryDir& home()
{
    static QTemporaryDir dir;
    return dir;
}

QString confPath()
{
    return home().filePath(QStringLiteral("config/Vela/vela-shell.conf"));
}


// A logo only in an icon theme ("themed-logo", a PNG: whether Qt reads SVG
// icons depends on its plugins), and an os-release naming it.
QString themedOsRelease(const QString& root)
{
    QDir().mkpath(root + QStringLiteral("/themes/test/apps"));
    QFile index(root + QStringLiteral("/themes/test/index.theme"));
    if (index.open(QIODevice::WriteOnly)) {
        index.write("[Icon Theme]\nName=Test\nDirectories=apps\n\n[apps]\nSize=48\nType=Fixed\n");
    }
    QImage logo(48, 48, QImage::Format_ARGB32);
    logo.fill(Qt::green);
    logo.save(root + QStringLiteral("/themes/test/apps/themed-logo.png"));
    const QString release = root + QStringLiteral("/themed-os-release");
    QFile file(release);
    if (file.open(QIODevice::WriteOnly)) {
        file.write("NAME=Themed\nLOGO=themed-logo\n");
    }
    return release;
}

} // namespace

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

    void followsStart()
    {
        Config config;
        QSignalSpy start(&config, &Config::startChanged);
        write("[start]\nshowPinned=false\n");
        QVERIFY(start.wait(3000));
        QVERIFY(!config.startPinned());

        // A picture there is: drawn; gone: the sail.
        const QString picture = home().filePath(QStringLiteral("start.png"));
        QFile file(picture);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("png");
        file.close();
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
        qputenv("VELA_OS_RELEASE", themedOsRelease(home().path()).toUtf8());
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
    QTemporaryDir& dir = home();
    if (!dir.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", dir.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_DATA_HOME", dir.filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_DATA_DIRS", dir.filePath(QStringLiteral("system")).toUtf8());
    qputenv("VELA_OS_RELEASE", dir.filePath(QStringLiteral("os-release")).toUtf8());
    qunsetenv("VELA_WALLPAPER");
    QDir().mkpath(dir.filePath(QStringLiteral("data/icons")));
    for (const auto& [name, text] : { std::pair { "os-release", "NAME=Test\nLOGO=test-logo\n" },
             std::pair { "data/icons/test-logo.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>" } }) {
        QFile file(dir.filePath(QLatin1String(name)));
        if (!file.open(QIODevice::WriteOnly)) {
            return 1;
        }
        file.write(text);
    }

    QGuiApplication app(argc, argv); // QIcon, as starticon.h asks
    QGuiApplication::setApplicationName(QStringLiteral("vela-shell"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    ConfigTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "config_test.moc"
