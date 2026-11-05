// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings' Preferences (src/preferences.h), Personalization's choices that
// live in vela-shell.conf: what they read, the keys they write, when they
// say they changed. Everything is in a directory of its own (the XDG
// directories, the runtime directory with no compositor's socket in it), and
// only setters that write vela-shell.conf are called: none starts a process
// or talks to a compositor.

#include "preferences.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

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

class PreferencesTest : public QObject {
    Q_OBJECT

    static QVariant saved(const QString& key)
    {
        QSettings settings(confPath(), QSettings::IniFormat);
        return settings.value(key);
    }
    static void write(const QByteArray& text)
    {
        QFile file(confPath());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(text);
    }
    static QString makeFile(const QString& name)
    {
        const QString path = home().filePath(name);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write("png");
        }
        return path;
    }

private slots:
    void init()
    {
        QDir().mkpath(QFileInfo(confPath()).absolutePath());
        // The apps' mode set: otherwise Preferences asks gsettings.
        write("[appearance]\nappTheme=dark\n");
    }

    void defaultsBeforeAnyChoice()
    {
        Preferences prefs;
        QVERIFY(prefs.desktopIcons());
        QVERIFY(prefs.startPinned());
        QVERIFY(prefs.clockDate());
        QVERIFY(prefs.clock24());
        QVERIFY(!prefs.clockSeconds());
        QVERIFY(prefs.notificationCount());
        QCOMPARE(prefs.timeFormat(), QStringLiteral("HH:mm"));
        QCOMPARE(prefs.startIcon(), QStringLiteral("vela"));
        QCOMPARE(prefs.startIconSource(), QString());
        QCOMPARE(prefs.distributionName(), QStringLiteral("Test Linux 2"));
        QCOMPARE(prefs.distributionLogo(), home().filePath(QStringLiteral("data/pixmaps/test-logo.png")));
    }

    void readsTheFile()
    {
        write("[appearance]\nappTheme=dark\n[desktop]\nshowIcons=false\n[start]\nshowPinned=false\nicon=distribution\n"
              "[clock]\nhours=12\n[taskbar]\nshowDate=false\nclockSeconds=true\nnotificationCount=false\n");
        Preferences prefs;
        QVERIFY(!prefs.desktopIcons());
        QVERIFY(!prefs.startPinned());
        QVERIFY(!prefs.clockDate());
        QVERIFY(!prefs.clock24());
        QVERIFY(prefs.clockSeconds());
        QVERIFY(!prefs.notificationCount());
        QCOMPARE(prefs.timeFormat(), QStringLiteral("h:mm AP"));
        QCOMPARE(prefs.startIcon(), QStringLiteral("distribution"));
        QCOMPARE(prefs.startIconSource(), prefs.distributionLogo());
        // Changed by another program (the desktop's menu), reread on reload().
        write("[appearance]\nappTheme=dark\n[desktop]\nshowIcons=true\n");
        QSignalSpy desktop(&prefs, &Preferences::desktopChanged);
        prefs.reload();
        QVERIFY(desktop.count() >= 1);
        QVERIFY(prefs.desktopIcons());
        QVERIFY(prefs.clock24());
    }

    void switchesWriteTheirKeys()
    {
        Preferences prefs;
        QSignalSpy clock(&prefs, &Preferences::clockChanged);
        QSignalSpy start(&prefs, &Preferences::startChanged);
        QSignalSpy desktop(&prefs, &Preferences::desktopChanged);

        prefs.setClock24(false);
        QCOMPARE(saved(QStringLiteral("clock/hours")).toInt(), 12);
        QCOMPARE(prefs.timeFormat(), QStringLiteral("h:mm AP"));
        prefs.setClock24(true);
        QCOMPARE(saved(QStringLiteral("clock/hours")).toInt(), 24);
        prefs.setClockSeconds(true);
        QCOMPARE(saved(QStringLiteral("taskbar/clockSeconds")).toBool(), true);
        prefs.setClockDate(false);
        QCOMPARE(saved(QStringLiteral("taskbar/showDate")).toBool(), false);
        prefs.setNotificationCount(false);
        QCOMPARE(saved(QStringLiteral("taskbar/notificationCount")).toBool(), false);
        QCOMPARE(clock.count(), 5);
        prefs.setStartPinned(false);
        QCOMPARE(saved(QStringLiteral("start/showPinned")).toBool(), false);
        QCOMPARE(start.count(), 1);
        prefs.setDesktopIcons(false);
        QCOMPARE(saved(QStringLiteral("desktop/showIcons")).toBool(), false);
        QCOMPARE(desktop.count(), 1);

        // The same value again: nothing written, nothing said.
        prefs.setClock24(true);
        prefs.setClockSeconds(true);
        prefs.setClockDate(false);
        prefs.setNotificationCount(false);
        prefs.setStartPinned(false);
        prefs.setDesktopIcons(false);
        QCOMPARE(clock.count(), 5);
        QCOMPARE(start.count(), 1);
        QCOMPARE(desktop.count(), 1);
    }

    void findsTheLogoOnceTheIconThemeIsSet()
    {
        // Vela.Controls sets the icon theme after Preferences starts
        // (main.cpp): a logo only there is offered once refreshStartIcon()
        // looks again.
        const QByteArray release = qgetenv("VELA_OS_RELEASE");
        qputenv("VELA_OS_RELEASE", themedOsRelease(home().path()).toUtf8());
        QIcon::setThemeSearchPaths({});
        write("[appearance]\nappTheme=dark\n[start]\nicon=distribution\n");
        Preferences prefs;
        QCOMPARE(prefs.distributionLogo(), QString());
        QCOMPARE(prefs.startIcon(), QStringLiteral("vela"));
        QSignalSpy start(&prefs, &Preferences::startChanged);
        QIcon::setThemeSearchPaths({ home().filePath(QStringLiteral("themes")) });
        QIcon::setThemeName(QStringLiteral("test"));
        prefs.refreshStartIcon();
        QCOMPARE(start.count(), 1);
        QCOMPARE(prefs.distributionName(), QStringLiteral("Themed"));
        QCOMPARE(prefs.distributionLogo(), QStringLiteral("themed-logo"));
        QCOMPARE(prefs.startIcon(), QStringLiteral("distribution"));
        QCOMPARE(prefs.startIconSource(), QStringLiteral("themed-logo"));
        prefs.refreshStartIcon();
        QCOMPARE(start.count(), 1);
        QIcon::setThemeSearchPaths({});
        qputenv("VELA_OS_RELEASE", release);
    }

    void startIcon()
    {
        Preferences prefs;
        QSignalSpy start(&prefs, &Preferences::startChanged);

        prefs.setStartIcon(QStringLiteral("orb")); // not a kind
        QCOMPARE(saved(QStringLiteral("start/icon")), QVariant());
        prefs.setStartIcon(QStringLiteral("vela")); // what it shows already
        QCOMPARE(start.count(), 0);

        prefs.setStartIcon(QStringLiteral("distribution"));
        QCOMPARE(prefs.startIcon(), QStringLiteral("distribution"));
        QCOMPARE(prefs.startIconSource(), prefs.distributionLogo());
        QCOMPARE(start.count(), 1);

        // A picture, as the file dialog gives it (a URL) or as a path.
        const QString picture = makeFile(QStringLiteral("sail.png"));
        prefs.setStartIcon(QStringLiteral("picture"), QUrl::fromLocalFile(picture).toString());
        QCOMPARE(prefs.startIcon(), QStringLiteral("picture"));
        QCOMPARE(prefs.startIconSource(), picture);
        QCOMPARE(saved(QStringLiteral("start/iconPicture")).toString(), picture);
        QCOMPARE(start.count(), 2);

        // Another picture: the same kind, a new source.
        const QString other = makeFile(QStringLiteral("other.png"));
        prefs.setStartIcon(QStringLiteral("picture"), other);
        QCOMPARE(prefs.startIconSource(), other);
        QCOMPARE(start.count(), 3);
        prefs.setStartIcon(QStringLiteral("picture"), picture);
        QCOMPARE(start.count(), 4);

        // One that isn't there, or isn't a path: ignored.
        prefs.setStartIcon(QStringLiteral("picture"), home().filePath(QStringLiteral("nowhere.png")));
        prefs.setStartIcon(QStringLiteral("picture"), QStringLiteral("relative.png"));
        QCOMPARE(prefs.startIconSource(), picture);
        QCOMPARE(start.count(), 4);

        // Back to the sail, then "Picture" without a file: the one before.
        prefs.setStartIcon(QStringLiteral("vela"));
        QCOMPARE(prefs.startIconSource(), QString());
        QCOMPARE(saved(QStringLiteral("start/iconPicture")).toString(), picture);
        prefs.setStartIcon(QStringLiteral("picture"));
        QCOMPARE(prefs.startIconSource(), picture);
        QCOMPARE(start.count(), 6);

        // The picture gone: what the taskbar shows, the sail.
        QFile::remove(picture);
        prefs.reload();
        QCOMPARE(prefs.startIcon(), QStringLiteral("vela"));
        QCOMPARE(prefs.startIconSource(), QString());
    }
};

int main(int argc, char** argv)
{
    QTemporaryDir& dir = home();
    if (!dir.isValid()) {
        return 1;
    }
    for (const auto& [name, leaf] : { std::pair { "HOME", "home" }, std::pair { "XDG_CONFIG_HOME", "config" },
             std::pair { "XDG_DATA_HOME", "data" }, std::pair { "XDG_CACHE_HOME", "cache" },
             std::pair { "XDG_STATE_HOME", "state" }, std::pair { "XDG_RUNTIME_DIR", "runtime" },
             std::pair { "XDG_DATA_DIRS", "system" } }) {
        const QString path = dir.filePath(QLatin1String(leaf));
        QDir().mkpath(path);
        qputenv(name, path.toUtf8());
    }
    QFile::setPermissions(dir.filePath(QStringLiteral("runtime")), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    // No compositor answers here.
    qputenv("WAYLAND_DISPLAY", "vela-preferences-test");
    qputenv("VELA_OS_RELEASE", dir.filePath(QStringLiteral("os-release")).toUtf8());
    QDir().mkpath(dir.filePath(QStringLiteral("data/pixmaps")));
    for (const auto& [name, text] : { std::pair { "os-release", "NAME=Test\nPRETTY_NAME=\"Test Linux 2\"\nLOGO=test-logo\n" },
             std::pair { "data/pixmaps/test-logo.png", "png" } }) {
        QFile file(dir.filePath(QLatin1String(name)));
        if (!file.open(QIODevice::WriteOnly)) {
            return 1;
        }
        file.write(text);
    }

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("vela-settings"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    PreferencesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "preferences_test.moc"
