// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings' Preferences (src/preferences.h), Personalization's choices that
// live in vela-shell.conf: what they read, the keys they write, when they
// say they changed. Everything is in a directory of its own (the XDG
// directories, the runtime directory with no compositor's socket in it), and
// only setters that write vela-shell.conf are called: none starts a process
// or talks to a compositor.

#include "defaults.h"
#include "preferences.h"
#include "privatehome.h"

#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QProcess>
#include <QSettings>
#include <QScopeGuard>
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

    // Values written before the config path was blocked must stay put.
    static void expectRejectedChanges(Preferences& prefs)
    {
        prefs.setNightTemperature(3000);
        QCOMPARE(prefs.nightTemperature(), 4500);
        prefs.setTearing(true);
        QVERIFY(!prefs.tearing());
        prefs.setVrr(QStringLiteral("always"));
        QCOMPARE(prefs.vrr(), QStringLiteral("never"));
        prefs.setDirectScanout(QStringLiteral("auto"));
        QCOMPARE(prefs.directScanout(), QStringLiteral("no"));
        prefs.setMagnifierStep(200);
        QCOMPARE(prefs.magnifierStep(), 150);
        prefs.setColorFilter(false);
        QVERIFY(prefs.colorFilter());
        prefs.setStickyKeys(false);
        QVERIFY(prefs.stickyKeys());
        prefs.setAnimationEffects(true);
        QVERIFY(!prefs.animationEffects());
        prefs.setMouseSpeed(3);
        QCOMPARE(prefs.mouseSpeed(), 12);
        prefs.setTouchpad(true);
        QVERIFY(!prefs.touchpad());
    }

private slots:
    /** Missing desktop helpers must produce a diagnostic without opening another app. */
    void missingFolderOpener()
    {
        const QByteArray originalPath = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", originalPath); });
        const QString bin = home().filePath(QStringLiteral("empty-bin"));
        QVERIFY(QDir().mkpath(bin));
        qputenv("PATH", bin.toUtf8());
        Preferences prefs;
        QTest::ignoreMessage(QtWarningMsg,
            "vela-settings: cannot open themes folder: xdg-open is unavailable (install xdg-utils)");
        prefs.openThemesFolder(QString());
        QVERIFY(QDir(home().filePath(QStringLiteral("config/vela/themes"))).exists());
    }

    /** An executable whose interpreter is unavailable must report its launch failure. */
    void brokenFolderOpener()
    {
        const QByteArray originalPath = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", originalPath); });
        const QString bin = home().filePath(QStringLiteral("broken-bin"));
        const QString opener = bin + QStringLiteral("/xdg-open");
        QVERIFY(writeFile(QStringLiteral("broken-bin/xdg-open"),
            "#!" + home().filePath(QStringLiteral("missing-interpreter")).toUtf8() + '\n'));
        QVERIFY(QFile::setPermissions(opener, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        qputenv("PATH", bin.toUtf8());
        Preferences prefs;
        const QString directory = home().filePath(QStringLiteral("config/vela/themes"));
        const QByteArray warning = QStringLiteral("vela-settings: failed to start xdg-open for %1").arg(directory).toUtf8();
        QTest::ignoreMessage(QtWarningMsg, warning.constData());
        prefs.openThemesFolder(QString());
    }

    /** The external opener receives the requested surface directory as one argument. */
    void folderOpenerGetsRequestedDirectory()
    {
        const QByteArray originalPath = qgetenv("PATH");
        const auto restore = qScopeGuard([&] {
            qputenv("PATH", originalPath);
            qunsetenv("VELA_TEST_FOLDER_PROBE");
        });
        const QString bin = home().filePath(QStringLiteral("working-bin"));
        const QString opener = bin + QStringLiteral("/xdg-open");
        QVERIFY(writeFile(QStringLiteral("working-bin/xdg-open"),
            "#!/bin/sh\nprintf '%s' \"$1\" > \"$VELA_TEST_FOLDER_PROBE\"\n"));
        QVERIFY(QFile::setPermissions(opener, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        qputenv("PATH", bin.toUtf8());
        const QString probe = home().filePath(QStringLiteral("folder-probe"));
        qputenv("VELA_TEST_FOLDER_PROBE", probe.toUtf8());
        Preferences prefs;
        prefs.openThemesFolder(QStringLiteral("taskbar"));
        const auto contents = [&] {
            QFile file(probe);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        QTRY_COMPARE_WITH_TIMEOUT(contents(),
            home().filePath(QStringLiteral("config/vela/themes/taskbar")).toUtf8(), 5000);
    }

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
        QVERIFY(prefs.silenceWhileSharing());
        QCOMPARE(prefs.timeFormat(), QStringLiteral("HH:mm"));
        QCOMPARE(prefs.startIcon(), QStringLiteral("vela"));
        QCOMPARE(prefs.startIconSource(), QString());
        QCOMPARE(prefs.distributionName(), QStringLiteral("Test Linux 2"));
        QCOMPARE(prefs.distributionLogo(), home().filePath(QStringLiteral("data/pixmaps/test-logo.png")));
    }

    void readsTheFile()
    {
        write("[appearance]\nappTheme=dark\n[desktop]\nshowIcons=false\n[start]\nshowPinned=false\nicon=distribution\n"
              "[clock]\nhours=12\n[taskbar]\nshowDate=false\nclockSeconds=true\nnotificationCount=false\n"
              "[notifications]\nsilenceWhileSharing=false\n");
        Preferences prefs;
        QVERIFY(!prefs.silenceWhileSharing());
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

    // appearance/accentSource (shell/src/accentsource.h): the theme decides
    // until the user chooses; only the three names are written, and a value
    // another program wrote is reread.
    void accentSource()
    {
        Preferences prefs;
        QCOMPARE(prefs.accentSource(), QStringLiteral("theme"));
        QSignalSpy accent(&prefs, &Preferences::accentChanged);
        prefs.setAccentSource(QStringLiteral("wallpaper"));
        QCOMPARE(prefs.accentSource(), QStringLiteral("wallpaper"));
        QCOMPARE(saved(QStringLiteral("appearance/accentSource")).toString(), QStringLiteral("wallpaper"));
        QCOMPARE(accent.count(), 1);
        prefs.setAccentSource(QStringLiteral("wallpaper")); // the same: nothing
        QCOMPARE(accent.count(), 1);
        prefs.setAccentSource(QStringLiteral("sunset")); // not a source: refused
        QCOMPARE(prefs.accentSource(), QStringLiteral("wallpaper"));
        QCOMPARE(saved(QStringLiteral("appearance/accentSource")).toString(), QStringLiteral("wallpaper"));
        prefs.setAccentSource(QStringLiteral("chosen"));
        QCOMPARE(saved(QStringLiteral("appearance/accentSource")).toString(), QStringLiteral("chosen"));
        // Choosing a color leaves the source as it is.
        prefs.setAccent(QColor(QStringLiteral("#ff8c00")));
        QCOMPARE(prefs.accentSource(), QStringLiteral("chosen"));
        QCOMPARE(saved(QStringLiteral("appearance/accent")).toString(), QStringLiteral("#ff8c00"));

        write("[appearance]\nappTheme=dark\naccentSource=wallpaper\n");
        prefs.reload();
        QCOMPARE(prefs.accentSource(), QStringLiteral("wallpaper"));
        write("[appearance]\nappTheme=dark\naccentSource=bogus\n");
        prefs.reload();
        QCOMPARE(prefs.accentSource(), QStringLiteral("theme"));
    }

    // Another program changing only the source: Colors hears it once (the
    // accent's signal), and not again for a reread that changes nothing; an
    // empty source is refused, a broken accent is Vela's.
    void accentSourceChangedElsewhere()
    {
        write("[appearance]\nappTheme=dark\naccent=#ff8c00\naccentSource=theme\n");
        Preferences prefs;
        QSignalSpy accent(&prefs, &Preferences::accentChanged);
        write("[appearance]\nappTheme=dark\naccent=#ff8c00\naccentSource=chosen\n");
        prefs.reload();
        QCOMPARE(prefs.accentSource(), QStringLiteral("chosen"));
        QCOMPARE(prefs.accent(), QColor(QStringLiteral("#ff8c00")));
        QCOMPARE(accent.count(), 1);
        prefs.reload();
        QCOMPARE(accent.count(), 1);
        prefs.setAccentSource(QString());
        QCOMPARE(prefs.accentSource(), QStringLiteral("chosen"));
        QCOMPARE(accent.count(), 1);
        write("[appearance]\nappTheme=dark\naccent=not a color\naccentSource=chosen\n");
        prefs.reload();
        QCOMPARE(prefs.accent(), vela::defaults::accent());
        QCOMPARE(accent.count(), 2);
    }

    // docs/battery-notifications.md §6.
    void batterySettings()
    {
        {
            Preferences prefs;
            QVERIFY(prefs.batteryNotify());
            QCOMPARE(prefs.batteryLowLevel(), 20);
            QVERIFY(prefs.deviceBatteryNotify());
            QCOMPARE(prefs.batteryLowLevels(), (QVariantList { 10, 15, 20, 25, 30, 40, 50 }));
            QSignalSpy battery(&prefs, &Preferences::batteryChanged);
            prefs.setBatteryNotify(false);
            QCOMPARE(saved(QStringLiteral("battery/notify")).toBool(), false);
            prefs.setBatteryLowLevel(30);
            QCOMPARE(saved(QStringLiteral("battery/lowLevel")).toInt(), 30);
            prefs.setBatteryLowLevel(2); // out of range: refused
            QCOMPARE(prefs.batteryLowLevel(), 30);
            prefs.setDeviceBatteryNotify(false);
            QCOMPARE(saved(QStringLiteral("battery/devices")).toBool(), false);
            QCOMPARE(battery.count(), 3);
            prefs.setDeviceBatteryNotify(false);
            QCOMPARE(battery.count(), 3);
        }
        // A level set by hand is offered among the others, in its place.
        // (Each file a different size: QSettings rereads by size and time.)
        write("[appearance]\nappTheme=dark\n[battery]\nlowLevel=12\n");
        Preferences prefs;
        QCOMPARE(prefs.batteryLowLevel(), 12);
        QCOMPARE(prefs.batteryLowLevels(), (QVariantList { 10, 12, 15, 20, 25, 30, 40, 50 }));
        write("[appearance]\nappTheme=dark\n[battery]\nlowLevel=8\n");
        prefs.reload();
        QCOMPARE(prefs.batteryLowLevels(), (QVariantList { 8, 10, 15, 20, 25, 30, 40, 50 }));
        write("[appearance]\nappTheme=dark\n[battery]\nlowLevel=80\nnotify=true\n");
        prefs.reload();
        QCOMPARE(prefs.batteryLowLevels().last().toInt(), 80);
        write("[appearance]\nappTheme=dark\n[battery]\nlowLevel=100\n\n");
        prefs.reload();
        QCOMPARE(prefs.batteryLowLevel(), 20);
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
        QSignalSpy dnd(&prefs, &Preferences::doNotDisturbChanged);
        prefs.setSilenceWhileSharing(false);
        QCOMPARE(saved(QStringLiteral("notifications/silenceWhileSharing")).toBool(), false);
        QCOMPARE(dnd.count(), 1);
        prefs.setSilenceWhileSharing(false);
        QCOMPARE(dnd.count(), 1);

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
        QVERIFY(prefs.saveError().contains(home().filePath(QStringLiteral("config/vela/vela.conf"))));
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

    void compositorSettingsRoundTrip()
    {
        const QString dir = home().filePath(QStringLiteral("config/vela"));
        QDir(dir).removeRecursively();
        QVERIFY(QDir().mkpath(dir));
        Preferences prefs;
        prefs.setScreenOffMinutes(4);
        prefs.setLockOnIdle(false);
        prefs.setLanguage(QStringLiteral("it"));
        prefs.setVisualTheme(QStringLiteral("Pill"));
        prefs.setSurfaceStyle(QStringLiteral("taskbar"), QStringLiteral("Dock"));
        prefs.resetThemes();
        prefs.setThemeAndStyles(QStringLiteral("Pill"), { { QStringLiteral("taskbar"), QStringLiteral("Dock") } });
        prefs.addKeyboardLayout(QStringLiteral("us"), QString());
        prefs.addKeyboardLayout(QStringLiteral("it"), QStringLiteral("nodeadkeys"));
        prefs.moveKeyboardLayoutUp(1);
        prefs.removeKeyboardLayout(0);
        prefs.setRepeatDelay(500);
        prefs.setRepeatRate(40);
        prefs.setNightLight(true);
        prefs.setNightTemperature(4500);
        prefs.setNightNvidiaFix(true);
        prefs.setNightSchedule(QStringLiteral("yes"));
        prefs.setNightFrom(QStringLiteral("20:15"));
        prefs.setNightTo(QStringLiteral("06:45"));
        prefs.setTearing(false);
        prefs.setVrr(QStringLiteral("never"));
        prefs.setDirectScanout(QStringLiteral("no"));
        prefs.setMagnifierStep(150);
        prefs.setColorFilter(true);
        prefs.setColorFilterKind(QStringLiteral("inverted"));
        prefs.setColorFilterShortcut(true);
        prefs.setStickyKeys(true);
        prefs.setAnimationEffects(false);
        prefs.setAnimationDuration(80);
        prefs.setMouseSpeed(12);
        prefs.setMousePrecision(false);
        prefs.setMouseLeftHanded(true);
        prefs.setWheelLines(5);
        prefs.setTouchpad(false);
        prefs.setTouchpadWithMouse(false);
        prefs.setTouchpadSpeed(12);
        prefs.setTouchpadTap(false);
        prefs.setTouchpadNatural(false);
        prefs.setThreeFingers(QStringLiteral("desktop"));
        prefs.setFourFingers(QStringLiteral("none"));
        QCOMPARE(prefs.saveError(), QString());
        QCOMPARE(prefs.screenOffMinutes(), 4);
        QCOMPARE(prefs.visualTheme(), QStringLiteral("Pill"));
        QCOMPARE(prefs.surfaceStyles().value(QStringLiteral("taskbar")).toString(), QStringLiteral("Dock"));
        QCOMPARE(prefs.keyboardLayouts().size(), 1);
        QCOMPARE(prefs.nightLight(), true);
        QCOMPARE(prefs.directScanout(), QStringLiteral("no"));
        QFile conf(dir + QStringLiteral("/vela.conf"));
        QVERIFY(conf.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(conf.readAll());
        conf.close();
        QVERIFY(text.contains(QStringLiteral("language=it")));
        QVERIFY(text.contains(QStringLiteral("keyboard-layout=us")));
        QVERIFY(text.contains(QStringLiteral("night-light=yes")));
        QVERIFY(text.contains(QStringLiteral("mouse-primary-button=right")));

        // An immutable file still reads, but QSaveFile cannot replace it.
        // That is the reload path. chattr is optional; the file-blocker below
        // covers the same setters when the file cannot be reread.
        const bool frozen = geteuid() == 0
            && QProcess::execute(QStringLiteral("chattr"), { QStringLiteral("+i"), conf.fileName() }) == 0;
        const auto thaw = qScopeGuard([frozen, path = conf.fileName()] {
            if (frozen) {
                QProcess::execute(QStringLiteral("chattr"), { QStringLiteral("-i"), path });
            }
        });
        if (frozen) {
            prefs.setScreenOffMinutes(9);
            QCOMPARE(prefs.screenOffMinutes(), 4);
            QVERIFY(!prefs.saveError().isEmpty());
            prefs.setLockOnIdle(true);
            QVERIFY(!prefs.lockOnIdle());
            prefs.setLanguage(QStringLiteral("en"));
            QCOMPARE(prefs.language(), QStringLiteral("it"));
            prefs.setVisualTheme(QStringLiteral("Fluent"));
            QCOMPARE(prefs.visualTheme(), QStringLiteral("Pill"));
            prefs.setNightLight(false);
            QVERIFY(prefs.nightLight());
            expectRejectedChanges(prefs);
        }

        // A file where the config directory should be. Root can still write
        // into a mode-0555 directory, which is what the CI runners are, so
        // the block has to be the path itself.
        const QString saved = text;
        QVERIFY(QDir(dir).removeRecursively());
        QVERIFY(writeFile(QStringLiteral("config/vela"), "not a directory\n"));
        prefs.setScreenOffMinutes(9);
        QCOMPARE(prefs.screenOffMinutes(), 4);
        QVERIFY(!prefs.saveError().isEmpty());
        prefs.setNightLight(false);
        QVERIFY(prefs.nightLight());
        prefs.setLanguage(QStringLiteral("en"));
        QCOMPARE(prefs.language(), QStringLiteral("it"));
        prefs.addKeyboardLayout(QStringLiteral("de"), QString());
        QCOMPARE(prefs.keyboardLayouts().size(), 1);
        prefs.setVisualTheme(QStringLiteral("Fluent"));
        QCOMPARE(prefs.visualTheme(), QStringLiteral("Pill"));
        prefs.resetThemes();
        QCOMPARE(prefs.visualTheme(), QStringLiteral("Pill"));
        prefs.setLockOnIdle(true);
        QVERIFY(!prefs.lockOnIdle());
        expectRejectedChanges(prefs);

        QVERIFY(QFile::remove(dir));
        QVERIFY(QDir().mkpath(dir));
        QVERIFY(writeFile(QStringLiteral("config/vela/vela.conf"), saved.toUtf8()));
        prefs.setLanguage(QStringLiteral("en"));
        QCOMPARE(prefs.language(), QStringLiteral("en"));
        QCOMPARE(prefs.saveError(), QString());
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
