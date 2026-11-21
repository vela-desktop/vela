// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Settings' Preferences (src/preferences.h), Personalization's choices that
// live in vela-shell.conf: what they read, the keys they write, when they
// say they changed. Everything is in a directory of its own (the XDG
// directories, the runtime directory with no compositor's socket in it), and
// appearance commands use private helpers. No host configuration or live
// compositor is used.

#include "defaults.h"
#include "controls.h"
#include "preferences.h"
#include "privatehome.h"

#include <unistd.h>
#include <utility>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMetaProperty>
#include <QPalette>
#include <QTextStream>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QtQml/qqmlextensionplugin.h>
#include <QSettings>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin)

using namespace vela::test;

// A file in place of the configuration directory blocks writes even on
// root CI runners. Keep the saved directory for restart/recovery assertions.
class BlockedDirectory {
public:
    explicit BlockedDirectory(QString path = QFileInfo(confPath()).absolutePath())
        : m_path(std::move(path)), m_backup(m_path + QStringLiteral("-saved")) {}
    ~BlockedDirectory() { restore(); }
    bool block()
    {
        if (!QDir().rename(m_path, m_backup)) return false;
        m_blocked = true;
        QFile blocker(m_path);
        return blocker.open(QIODevice::WriteOnly) && blocker.write("blocked\n") == 8;
    }
    bool restore()
    {
        if (!m_blocked) return true;
        if (!QFile::remove(m_path) || !QDir().rename(m_backup, m_path)) return false;
        m_blocked = false;
        return true;
    }
private:
    QString m_path;
    QString m_backup;
    bool m_blocked = false;
};

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

    // A separate process cannot see QSettings' shared in-process cache.
    static void expectFreshShell(const QVariantMap& expected, const QStringList& absent = {})
    {
        QProcess reader;
        reader.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--read-shell") });
        QVERIFY(reader.waitForFinished(5000));
        QCOMPARE(reader.exitStatus(), QProcess::NormalExit);
        QCOMPARE(reader.exitCode(), 0);
        const QJsonObject values = QJsonDocument::fromJson(reader.readAllStandardOutput()).object();
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            QVERIFY(values.contains(it.key()));
            const QVariant actual = values.value(it.key()).toVariant();
            // INI keeps booleans and one-element string lists as strings;
            // apply the same type conversion as the consuming preferences.
            if (it.value().metaType().id() == QMetaType::Bool) {
                QCOMPARE(actual.toBool(), it.value().toBool());
            } else if (it.value().metaType().id() == QMetaType::QStringList) {
                QCOMPARE(actual.toStringList(), it.value().toStringList());
            } else {
                QCOMPARE(values.value(it.key()), QJsonValue::fromVariant(it.value()));
            }
        }
        for (const QString& key : absent) {
            QVERIFY(!values.contains(key));
        }
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
    void taskbarSettings()
    {
        write("[appearance]\nappTheme=dark\n"
              "[taskbar]\nwindowsFrom=invalid\nappsOn=invalid\nclickActive=invalid\ntrayShown=discord,,discord\n"
              "attention=unknown\n");
        Preferences prefs;
        QCOMPARE(prefs.taskbarWindowsFrom(), QStringLiteral("desktop"));
        QCOMPARE(prefs.taskbarPosition(), QStringLiteral("bottom"));
        QCOMPARE(prefs.taskbarAppsOn(), QStringLiteral("all"));
        QCOMPARE(prefs.taskbarClickActive(), QStringLiteral("previews"));
        QCOMPARE(prefs.taskbarTrayShown(), QStringList { QStringLiteral("discord") });
        QVERIFY(prefs.taskbarAttention());

        prefs.setTaskbarWindowsFrom(QStringLiteral("all"));
        prefs.setTaskbarPosition(QStringLiteral("diagonal"));
        prefs.setTaskbarPosition(QStringLiteral("left"));
        prefs.setTaskbarAppsOn(QStringLiteral("window"));
        prefs.setTaskbarClickActive(QStringLiteral("next"));
        prefs.setTrayIconShown(QStringLiteral("steam"), true);
        prefs.setTrayIconShown(QStringLiteral("discord"), false);
        prefs.setTrayIconShown(QStringLiteral("telegram"), true);
        prefs.setTaskbarAttention(false);
        prefs.setTaskbarBadges(false);
        prefs.setTaskbarProgress(false);
        prefs.setTaskbarNumberHints(false);
        expectFreshShell({
            { "taskbar/windowsFrom", "all" },
            { "taskbar/position", "left" },
            { "taskbar/appsOn", "window" },
            { "taskbar/clickActive", "next" },
            { "taskbar/trayShown", QStringList { QStringLiteral("steam"), QStringLiteral("telegram") } },
            { "taskbar/attention", false },
            { "taskbar/badges", false },
            { "taskbar/progress", false },
            { "taskbar/numberHints", false },
        });
        // Unknown values, and the values already chosen, write nothing.
        QSignalSpy taskbar(&prefs, &Preferences::taskbarChanged);
        prefs.setTaskbarPosition(QStringLiteral("left"));
        prefs.setTaskbarWindowsFrom(QStringLiteral("all"));
        prefs.setTaskbarWindowsFrom(QStringLiteral("everywhere"));
        prefs.setTaskbarAppsOn(QStringLiteral("window"));
        prefs.setTaskbarAppsOn(QStringLiteral("nowhere"));
        prefs.setTaskbarClickActive(QStringLiteral("next"));
        prefs.setTaskbarClickActive(QStringLiteral("twice"));
        prefs.setTrayIconShown(QStringLiteral("steam"), true);
        prefs.setTrayIconShown(QStringLiteral("discord"), false);
        prefs.setTrayIconShown(QString(), true);
        prefs.setTaskbarAttention(false);
        prefs.setTaskbarBadges(false);
        prefs.setTaskbarProgress(false);
        prefs.setTaskbarNumberHints(false);
        prefs.setSuperNumberKeys(QStringLiteral("apps"));
        prefs.setSuperNumberKeys(QStringLiteral("letters"));
        QCOMPARE(taskbar.count(), 0);
        prefs.setSuperNumberKeys(QStringLiteral("desktops"));
        QCOMPARE(taskbar.count(), 1);
        Preferences reread;
        QCOMPARE(reread.superNumberKeys(), QStringLiteral("desktops"));

        BlockedDirectory blocker;
        QVERIFY(blocker.block());
        prefs.setTrayIconShown(QStringLiteral("steam"), false);
        QCOMPARE(prefs.taskbarTrayShown(), (QStringList { QStringLiteral("steam"), QStringLiteral("telegram") }));
        QVERIFY(blocker.restore());
    }

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

    // Personalization > Context menu: menus/hidden holds the hidden keys; a
    // group hides and shows at once; "Show all" empties it.
    void hiddenMenuEntries()
    {
        write("[appearance]\nappTheme=dark\n[menus]\nhidden=file.copy\n");
        Preferences prefs;
        QCOMPARE(prefs.hiddenMenuEntries(), QStringList { QStringLiteral("file.copy") });
        prefs.setMenuEntriesHidden({ QStringLiteral("taskbar.recent"), QStringLiteral("taskbar.recentHeader") }, true);
        const QStringList three { QStringLiteral("file.copy"), QStringLiteral("taskbar.recent"),
            QStringLiteral("taskbar.recentHeader") };
        QCOMPARE(prefs.hiddenMenuEntries(), three);
        QCOMPARE(saved(QStringLiteral("menus/hidden")).toStringList(), three);
        prefs.setMenuEntriesHidden({ QStringLiteral("file.copy") }, false);
        QCOMPARE(saved(QStringLiteral("menus/hidden")).toStringList(), three.mid(1));
        prefs.showAllMenuEntries();
        QVERIFY(prefs.hiddenMenuEntries().isEmpty());
        QVERIFY(!saved(QStringLiteral("menus/hidden")).isValid());
        Preferences again;
        QVERIFY(again.hiddenMenuEntries().isEmpty());
    }

    // Another writer hid Cut after Settings read the file: hiding Copy keeps
    // Cut hidden, and "Show all" brings back every key, the other writer's too.
    void hiddenMenuEntriesKeepOtherWritersKeys()
    {
        write("[appearance]\nappTheme=dark\n");
        Preferences prefs;
        QVERIFY(prefs.hiddenMenuEntries().isEmpty());
        write("[appearance]\nappTheme=dark\n[menus]\nhidden=file.cut\n");
        prefs.setMenuEntriesHidden({ QStringLiteral("file.copy") }, true);
        const QStringList both { QStringLiteral("file.cut"), QStringLiteral("file.copy") };
        QCOMPARE(saved(QStringLiteral("menus/hidden")).toStringList(), both);
        QCOMPARE(prefs.hiddenMenuEntries(), both);
        write("[appearance]\nappTheme=dark\n[menus]\nhidden=file.cut, file.copy, file.rename\n");
        prefs.showAllMenuEntries();
        QVERIFY(!saved(QStringLiteral("menus/hidden")).isValid());
        QVERIFY(prefs.hiddenMenuEntries().isEmpty());
        QCOMPARE(saved(QStringLiteral("appearance/appTheme")).toString(), QStringLiteral("dark"));
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

    void shellWriteFailure_data()
    {
        QTest::addColumn<QByteArray>("property");
        QTest::addColumn<QString>("key");
        QTest::addColumn<QVariant>("value");
        QTest::addColumn<bool>("present");
        struct Choice { const char* property; const char* key; QVariant value; };
        const QList<Choice> choices {
            { "accent", "appearance/accent", QColor(Qt::red) },
            { "accentSource", "appearance/accentSource", QStringLiteral("chosen") },
            { "shellTheme", "appearance/shellTheme", QStringLiteral("light") },
            { "taskbarAlignment", "taskbar/alignment", QStringLiteral("left") },
            { "endTask", "taskbar/endTask", false },
            { "taskView", "taskbar/taskView", false },
            { "taskbarAllScreens", "taskbar/allScreens", false },
            { "showDesktopCorner", "taskbar/showDesktopCorner", false },
            { "taskbarPosition", "taskbar/position", QStringLiteral("top") },
            { "taskbarWindowsFrom", "taskbar/windowsFrom", QStringLiteral("all") },
            { "taskbarAppsOn", "taskbar/appsOn", QStringLiteral("window") },
            { "taskbarClickActive", "taskbar/clickActive", QStringLiteral("next") },
            { "taskbarAttention", "taskbar/attention", false },
            { "taskbarBadges", "taskbar/badges", false },
            { "taskbarProgress", "taskbar/progress", false },
            { "taskbarNumberHints", "taskbar/numberHints", false },
            { "desktopIcons", "desktop/showIcons", false },
            { "startPinned", "start/showPinned", false },
            { "clockDate", "taskbar/showDate", false },
            { "clock24", "clock/hours", false },
            { "clockSeconds", "taskbar/clockSeconds", true },
            { "notificationCount", "taskbar/notificationCount", false },
            { "doNotDisturb", "notifications/doNotDisturb", true },
            { "silenceWhileSharing", "notifications/silenceWhileSharing", false },
            { "batteryNotify", "battery/notify", false },
            { "batteryLowLevel", "battery/lowLevel", 10 },
            { "deviceBatteryNotify", "battery/devices", false },
            { "clipboardHistory", "clipboard/history", true },
        };
        for (const auto& choice : choices) {
            for (bool present : { false, true }) {
                const QByteArray name = QByteArray(choice.property) + (present ? "-existing" : "-absent");
                QTest::newRow(name.constData()) << QByteArray(choice.property) << QString::fromLatin1(choice.key)
                    << choice.value << present;
            }
        }
    }

    void shellWriteFailure()
    {
        QFETCH(QByteArray, property);
        QFETCH(QString, key);
        QFETCH(QVariant, value);
        QFETCH(bool, present);
        Preferences prefs;
        const QMetaProperty member = prefs.metaObject()->property(prefs.metaObject()->indexOfProperty(property));
        QVERIFY(member.isWritable());
        const QVariant original = member.read(&prefs);
        QVERIFY(original != value);
        if (present) {
            QSettings initial(confPath(), QSettings::IniFormat);
            initial.setValue(key, property == "clock24" ? QVariant(24) : original.toString());
            initial.sync();
            QCOMPARE(initial.status(), QSettings::NoError);
        }
        const QVariant previous = saved(key);
        QSignalSpy changed(&prefs, member.notifySignal());
        BlockedDirectory blocked;
        QVERIFY(blocked.block());
        QVERIFY(member.write(&prefs, value));
        QCOMPARE(member.read(&prefs), original);
        QVERIFY(prefs.saveError().contains(confPath()));
        QVERIFY(!prefs.saveError().contains(QStringLiteral("/vela/vela.conf")));
        QVERIFY(changed.count() >= 1); // re-evaluate a control even when the value stayed put
        QVERIFY(blocked.restore());
        // A different writer and deferred syncs must not resurrect the failed value.
        {
            QSettings other(confPath(), QSettings::IniFormat);
            other.setValue(QStringLiteral("test/probe"), true);
            other.sync();
            QCOMPARE(other.status(), QSettings::NoError);
            QCOMPARE(other.contains(key), present);
            QCOMPARE(other.value(key).toString(), previous.toString());
        }
        QCoreApplication::processEvents();
        prefs.reload();
        QCOMPARE(member.read(&prefs), original);
        changed.clear();
        QVERIFY(member.write(&prefs, value));
        QCOMPARE(member.read(&prefs), value);
        QCOMPARE(prefs.saveError(), QString());
        QCOMPARE(changed.count(), 1);
        Preferences reloaded;
        QCOMPARE(member.read(&reloaded), value);
    }

    void startIconFailureDoesNotQueueEitherKey_data()
    {
        QTest::addColumn<bool>("pictureBefore");
        QTest::newRow("absent-keys") << false;
        QTest::newRow("existing-picture") << true;
    }

    void startIconFailureDoesNotQueueEitherKey()
    {
        QFETCH(bool, pictureBefore);
        const QString oldPicture = makeFile(QStringLiteral("saved-start.png"));
        const QString newPicture = makeFile(QStringLiteral("rejected-start.png"));
        Preferences prefs;
        if (pictureBefore) prefs.setStartIcon(QStringLiteral("picture"), oldPicture);
        const QString kind = prefs.startIcon();
        const QString source = prefs.startIconSource();
        BlockedDirectory blocked;
        QVERIFY(blocked.block());
        prefs.setStartIcon(QStringLiteral("picture"), newPicture);
        QCOMPARE(prefs.startIcon(), kind);
        QCOMPARE(prefs.startIconSource(), source);
        QVERIFY(!prefs.saveError().isEmpty());
        QVERIFY(blocked.restore());
        prefs.setDesktopIcons(false); // includes a sync after the path became writable
        prefs.refreshStartIcon();
        prefs.reload();
        QCOMPARE(prefs.startIcon(), kind);
        QCOMPARE(prefs.startIconSource(), source);
        if (pictureBefore) {
            expectFreshShell({ { QStringLiteral("start/icon"), QStringLiteral("picture") },
                { QStringLiteral("start/iconPicture"), oldPicture } });
        } else {
            expectFreshShell({}, { QStringLiteral("start/icon"), QStringLiteral("start/iconPicture") });
        }
        prefs.setStartIcon(QStringLiteral("picture"), newPicture);
        QCOMPARE(prefs.startIconSource(), newPicture);
        QCOMPARE(prefs.saveError(), QString());
        expectFreshShell({ { QStringLiteral("start/icon"), QStringLiteral("picture") },
            { QStringLiteral("start/iconPicture"), newPicture } });
    }

    void wallpaperFailureKeepsThePictureAndHistoryTogether()
    {
        const QString oldPicture = makeFile(QStringLiteral("saved-wallpaper.png"));
        const QString newPicture = makeFile(QStringLiteral("rejected-wallpaper.png"));
        Preferences prefs;
        prefs.setWallpaper(oldPicture);
        const QStringList recent = prefs.recentWallpapers();
        QSignalSpy changed(&prefs, &Preferences::wallpaperChanged);
        BlockedDirectory blocked;
        QVERIFY(blocked.block());
        prefs.setWallpaper(newPicture);
        QCOMPARE(prefs.wallpaper(), oldPicture);
        QCOMPARE(prefs.recentWallpapers(), recent);
        QVERIFY(changed.count() >= 1);
        QVERIFY(blocked.restore());
        prefs.setClockSeconds(true);
        expectFreshShell({ { QStringLiteral("appearance/wallpaper"), oldPicture },
            { QStringLiteral("appearance/recentWallpapers"), recent },
            { QStringLiteral("taskbar/clockSeconds"), true } });
        prefs.reload();
        QCOMPARE(prefs.wallpaper(), oldPicture);
        prefs.setWallpaper(newPicture);
        QCOMPARE(prefs.wallpaper(), newPicture);
        QCOMPARE(prefs.recentWallpapers().first(), newPicture);
        QCOMPARE(prefs.saveError(), QString());
    }

    void shellAndCompositorErrorsStayIndependent_data()
    {
        QTest::addColumn<bool>("shellFirst");
        QTest::newRow("repair-shell-first") << true;
        QTest::newRow("repair-compositor-first") << false;
    }

    void shellAndCompositorErrorsStayIndependent()
    {
        QFETCH(bool, shellFirst);
        QVERIFY(writeFile(QStringLiteral("config/vela/vela.conf"), "screen-off=10\n"));
        Preferences prefs;
        BlockedDirectory shell;
        BlockedDirectory compositor(home().filePath(QStringLiteral("config/vela")));
        QVERIFY(shell.block());
        QVERIFY(compositor.block());
        prefs.setClockSeconds(true);
        prefs.setScreenOffMinutes(3);
        QVERIFY(prefs.saveError().contains(confPath()));
        QVERIFY(prefs.saveError().contains(QStringLiteral("/vela/vela.conf")));
        prefs.reload(); // the compositor reload must not replace the shell error
        QVERIFY(prefs.saveError().contains(confPath()));
        if (shellFirst) {
            QVERIFY(shell.restore());
            prefs.setClockSeconds(true);
            QVERIFY(!prefs.saveError().contains(confPath()));
            QVERIFY(prefs.saveError().contains(QStringLiteral("/vela/vela.conf")));
            QVERIFY(compositor.restore());
            prefs.setScreenOffMinutes(3);
        } else {
            QVERIFY(compositor.restore());
            prefs.setScreenOffMinutes(3);
            QVERIFY(prefs.saveError().contains(confPath()));
            QVERIFY(!prefs.saveError().contains(QStringLiteral("/vela/vela.conf")));
            QVERIFY(shell.restore());
            prefs.setClockSeconds(true);
        }
        QCOMPARE(prefs.saveError(), QString());
    }

    void appModeFailureDoesNotLaunchAppearanceHelpers()
    {
        const QByteArray originalPath = qgetenv("PATH");
        const QString originalIcons = QIcon::themeName();
        const QStringList originalSearch = QIcon::themeSearchPaths();
        const QPalette originalPalette = QGuiApplication::palette();
        const auto restore = qScopeGuard([&] {
            qputenv("PATH", originalPath);
            qunsetenv("VELA_TEST_MODE_PROBE");
            QIcon::setThemeName(originalIcons);
            QIcon::setThemeSearchPaths(originalSearch);
            QGuiApplication::setPalette(originalPalette);
        });
        const QString bin = home().filePath(QStringLiteral("mode-bin"));
        const QString probe = home().filePath(QStringLiteral("mode-probe"));
        QVERIFY(QFile::remove(probe) || !QFileInfo::exists(probe));
        for (const QString& helper : { QStringLiteral("gsettings"), QStringLiteral("plasma-apply-colorscheme"),
                 QStringLiteral("kwriteconfig6") }) {
            const QString relative = QStringLiteral("mode-bin/") + helper;
            QVERIFY(writeFile(relative, "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$VELA_TEST_MODE_PROBE\"\n"));
            QVERIFY(QFile::setPermissions(home().filePath(relative),
                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        }
        qputenv("PATH", bin.toUtf8());
        qputenv("VELA_TEST_MODE_PROBE", probe.toUtf8());
        // No light/dark variants exist for this private icon theme, so a
        // successful test cannot launch the absolute plasma-changeicons helper.
        QIcon::setThemeName(QStringLiteral("preferences-test-icons"));
        QIcon::setThemeSearchPaths({ home().filePath(QStringLiteral("private-icons")) });
        Preferences prefs;
        BlockedDirectory blocked;
        QVERIFY(blocked.block());
        prefs.setAppTheme(QStringLiteral("light"));
        QCOMPARE(prefs.appTheme(), QStringLiteral("dark"));
        QVERIFY(!prefs.saveError().isEmpty());
        QCOMPARE(QIcon::themeName(), QStringLiteral("preferences-test-icons"));
        QCOMPARE(QGuiApplication::palette(), originalPalette);
        QTest::qWait(100);
        QVERIFY(!QFileInfo::exists(probe));
        QVERIFY(blocked.restore());
        prefs.setAppTheme(QStringLiteral("light"));
        QCOMPARE(prefs.appTheme(), QStringLiteral("light"));
        QCOMPARE(prefs.saveError(), QString());
        const auto calls = [&] {
            QFile file(probe);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        QTRY_VERIFY_WITH_TIMEOUT(calls().contains("set org.gnome.desktop.interface color-scheme prefer-light"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(calls().contains("BreezeLight"), 5000);
        expectFreshShell({ { QStringLiteral("appearance/appTheme"), QStringLiteral("light") } });
    }

    void failedWriteReloadsNewerPersistedShellValues()
    {
        if (geteuid() == 0) QSKIP("Root bypasses Unix write permissions; path blockers cover failed writes in CI.");
        Preferences prefs;
        QCOMPARE(prefs.accentSource(), QStringLiteral("theme"));
        // Another writer changed the file before this attempt, while this
        // Preferences object still shows the previous value.
        write("[appearance]\nappTheme=dark\naccentSource=wallpaper\n");
        const QString directory = QFileInfo(confPath()).absolutePath();
        const auto permissions = QFile::permissions(directory);
        const auto filePermissions = QFile::permissions(confPath());
        const auto restore = qScopeGuard([&] {
            QFile::setPermissions(directory, permissions);
            QFile::setPermissions(confPath(), filePermissions);
        });
        QVERIFY(QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QVERIFY(QFile::setPermissions(confPath(), QFileDevice::ReadOwner));
        prefs.setAccentSource(QStringLiteral("chosen"));
        QCOMPARE(prefs.accentSource(), QStringLiteral("wallpaper"));
        QVERIFY(prefs.saveError().contains(confPath()));
        expectFreshShell({ { QStringLiteral("appearance/accentSource"), QStringLiteral("wallpaper") } });
    }

    void failedWritesRestoreBoundSwitchAndChoice()
    {
        Preferences prefs;
        QQmlEngine engine;
        vela::controls::install(engine, Appearance::Mode::Apps);
        engine.rootContext()->setContextProperty(QStringLiteral("Prefs"), &prefs);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import Vela.Controls
            import "."
            Window {
                width: 480; height: 180; visible: true
                Switch {
                    objectName: "desktopSwitch"
                    x: 20; y: 20
                    checked: Prefs.desktopIcons
                    onToggled: Prefs.desktopIcons = checked
                }
                Choice {
                    objectName: "alignmentChoice"
                    x: 200; y: 20
                    model: ["Center", "Left"]
                    currentIndex: Prefs.taskbarAlignment === "left" ? 1 : 0
                    onChosen: index => Prefs.taskbarAlignment = index === 1 ? "left" : "center"
                }
            }
        )", QUrl::fromLocalFile(QStringLiteral(SETTINGS_QML_DIR "/PreferencesProbe.qml")));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* window = qobject_cast<QQuickWindow*>(object.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* toggle = window->findChild<QQuickItem*>(QStringLiteral("desktopSwitch"));
        auto* choice = window->findChild<QQuickItem*>(QStringLiteral("alignmentChoice"));
        QVERIFY(toggle && choice);
        const auto click = [window](QQuickItem* item) {
            const QPointF center = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
        };
        const auto chooseLeft = [&] {
            click(choice);
            auto* popup = choice->property("popup").value<QObject*>();
            QVERIFY(popup);
            QTRY_VERIFY(popup->property("visible").toBool());
            QTest::keyClick(window, Qt::Key_End);
            QTest::keyClick(window, Qt::Key_Return);
        };
        BlockedDirectory blocked;
        QVERIFY(blocked.block());
        click(toggle);
        QVERIFY(prefs.desktopIcons());
        QVERIFY(toggle->property("checked").toBool());
        QVERIFY(!prefs.saveError().isEmpty());
        chooseLeft();
        QCOMPARE(prefs.taskbarAlignment(), QStringLiteral("center"));
        QCOMPARE(choice->property("currentIndex").toInt(), 0);
        QVERIFY(blocked.restore());
        click(toggle);
        QVERIFY(!prefs.desktopIcons());
        QVERIFY(!toggle->property("checked").toBool());
        chooseLeft();
        QCOMPARE(prefs.taskbarAlignment(), QStringLiteral("left"));
        QCOMPARE(choice->property("currentIndex").toInt(), 1);
        QCOMPARE(prefs.saveError(), QString());
    }

    void failedWritesDoNotOverrideALaterExternalWriter_data()
    {
        QTest::addColumn<bool>("present");
        QTest::newRow("key-was-absent") << false;
        QTest::newRow("key-existed") << true;
    }

    void failedWritesDoNotOverrideALaterExternalWriter()
    {
        QFETCH(bool, present);
        if (present) write("[appearance]\nappTheme=dark\n[taskbar]\nalignment=center\n");
        Preferences prefs;
        BlockedDirectory blocked;
        QVERIFY(blocked.block());
        prefs.setTaskbarAlignment(QStringLiteral("left"));
        QCOMPARE(prefs.taskbarAlignment(), QStringLiteral("center"));
        QVERIFY(!prefs.saveError().isEmpty());
        QVERIFY(blocked.restore());
        QProcess writer;
        writer.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--write-shell"), confPath(),
            QStringLiteral("taskbar/alignment"), QStringLiteral("left") });
        QVERIFY(writer.waitForFinished(5000));
        QCOMPARE(writer.exitStatus(), QProcess::NormalExit);
        QCOMPARE(writer.exitCode(), 0);
        // Neither a deferred flush nor a later sync may replay the failed
        // update or a queued rollback over this legitimate external choice.
        QCoreApplication::processEvents();
        prefs.reload();
        QCOMPARE(prefs.taskbarAlignment(), QStringLiteral("left"));
        expectFreshShell({ { QStringLiteral("taskbar/alignment"), QStringLiteral("left") } });
    }

    void busyWriterLeavesTheConfigurationUntouched()
    {
        Preferences prefs;
        QLockFile otherWriter(confPath() + QStringLiteral(".lock"));
        QVERIFY(otherWriter.tryLock());
        prefs.setDesktopIcons(false);
        QVERIFY(prefs.desktopIcons());
        QVERIFY(prefs.saveError().contains(confPath()));
        otherWriter.unlock();
        expectFreshShell({}, { QStringLiteral("desktop/showIcons") });
        prefs.setDesktopIcons(false);
        QVERIFY(!prefs.desktopIcons());
        QCOMPARE(prefs.saveError(), QString());
    }

    void malformedShellFileIsNotOverwritten()
    {
        Preferences prefs;
        const QByteArray broken = "[appearance]\nappTheme=dark\n[broken\nvalue=1\n";
        write(broken);
        prefs.setDesktopIcons(false);
        QVERIFY(prefs.desktopIcons());
        QVERIFY(prefs.saveError().contains(confPath()));
        QFile file(confPath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), broken);
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

    void atMostFourKeyboardLayouts()
    {
        const QString dir = home().filePath(QStringLiteral("config/vela"));
        QDir(dir).removeRecursively();
        QVERIFY(QDir().mkpath(dir));
        Preferences prefs;
        for (const char* layout : { "it", "us", "de", "fr", "es" }) {
            prefs.addKeyboardLayout(QString::fromLatin1(layout), QString());
        }
        // XKB's v1 keymaps hold four (docs/keyboard.md §5.1): the fifth is
        // refused, not saved to be ignored.
        QCOMPARE(prefs.keyboardLayouts().size(), 4);
        QFile file(dir + QStringLiteral("/vela.conf"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("keyboard-layout=it,us,de,fr\n"));
        prefs.removeKeyboardLayout(3);
        prefs.addKeyboardLayout(QStringLiteral("es"), QString());
        QCOMPARE(prefs.keyboardLayouts().size(), 4);
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
    if (argc == 5 && QByteArray(argv[1]) == "--write-shell") {
        QCoreApplication app(argc, argv);
        QSettings settings(QString::fromLocal8Bit(argv[2]), QSettings::IniFormat);
        settings.setValue(QString::fromLocal8Bit(argv[3]), QString::fromLocal8Bit(argv[4]));
        settings.sync();
        return settings.status() == QSettings::NoError ? 0 : 1;
    }
    if (argc == 2 && QByteArray(argv[1]) == "--read-shell") {
        QCoreApplication app(argc, argv);
        QSettings settings(QStringLiteral("Vela"), QStringLiteral("vela-shell"));
        settings.setFallbacksEnabled(false);
        settings.sync();
        QVariantMap values;
        for (const QString& key : settings.allKeys()) values.insert(key, settings.value(key));
        QTextStream(stdout) << QJsonDocument::fromVariant(values).toJson(QJsonDocument::Compact);
        return settings.status() == QSettings::NoError ? 0 : 1;
    }

    // A distribution whose logo is a loose file in pixmaps/.
    if (!isolate()
        || !writeFile(QStringLiteral("os-release"), "NAME=Test\nPRETTY_NAME=\"Test Linux 2\"\nLOGO=test-logo\n")
        || !writeFile(QStringLiteral("data/pixmaps/test-logo.png"), "png")) {
        return 1;
    }

    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("vela-settings"));
    QGuiApplication::setOrganizationName(QStringLiteral("Vela"));
    PreferencesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "preferences_test.moc"
