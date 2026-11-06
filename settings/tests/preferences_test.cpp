// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings' Preferences (src/preferences.h), Personalization's choices that
// live in vela-shell.conf: what they read, the keys they write, when they
// say they changed. Everything is in a directory of its own (the XDG
// directories, the runtime directory with no compositor's socket in it), and
// only setters that write vela-shell.conf are called: none starts a process
// or talks to a compositor.

#include "preferences.h"
#include "privatehome.h"

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

using namespace vela::test;

class PreferencesTest : public QObject {
    Q_OBJECT

    static QVariant saved(const QString& key)
    {
        QSettings settings(confPath(), QSettings::IniFormat);
        return settings.value(key);
    }
    static void write(const QByteArray& text)
    {
        QVERIFY(writeFile(QStringLiteral("config/Vela/vela-shell.conf"), text));
    }
    static QString makeFile(const QString& name)
    {
        const QString path = home().filePath(name);
        paint(path, Qt::red);
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
        qputenv("VELA_OS_RELEASE", themedOsRelease().toUtf8());
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

        // One that isn't there, isn't a path or isn't a picture: ignored.
        const QString text = home().filePath(QStringLiteral("notes.png"));
        QFile notes(text);
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.write("not a picture");
        notes.close();
        prefs.setStartIcon(QStringLiteral("picture"), text);
        QCOMPARE(prefs.startIconSource(), picture);
        QCOMPARE(saved(QStringLiteral("start/iconPicture")).toString(), picture);
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

    void compositorSaveFailureRestoresTheValue()
    {
        // A file where the config directory should be: the write cannot commit.
        const QString blocker = home().filePath(QStringLiteral("config/vela"));
        QDir(blocker).removeRecursively();
        QVERIFY(writeFile(QStringLiteral("config/vela"), "not a directory\n"));
        Preferences prefs;
        QCOMPARE(prefs.screenOffMinutes(), 10);
        QCOMPARE(prefs.saveError(), QString());
        QSignalSpy idle(&prefs, &Preferences::idleChanged);
        QSignalSpy error(&prefs, &Preferences::saveErrorChanged);
        prefs.setScreenOffMinutes(3);
        QCOMPARE(prefs.screenOffMinutes(), 10);
        QVERIFY(!prefs.saveError().isEmpty());
        QCOMPARE(idle.count(), 1);
        QCOMPARE(error.count(), 1);

        QVERIFY(QFile::remove(blocker));
        QVERIFY(QDir().mkpath(home().filePath(QStringLiteral("config/vela"))));
        prefs.setScreenOffMinutes(3);
        QCOMPARE(prefs.screenOffMinutes(), 3);
        QCOMPARE(prefs.saveError(), QString());
        QFile file(home().filePath(QStringLiteral("config/vela/vela.conf")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(file.readAll()).contains(QStringLiteral("screen-off=3")));
    }
};

int main(int argc, char** argv)
{
    // A distribution whose logo is a loose file in pixmaps/.
    if (!isolate()
        || !writeFile(QStringLiteral("os-release"), "NAME=Test\nPRETTY_NAME=\"Test Linux 2\"\nLOGO=test-logo\n")
        || !writeFile(QStringLiteral("data/pixmaps/test-logo.png"), "png")) {
        return 1;
    }

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("vela-settings"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    PreferencesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "preferences_test.moc"
