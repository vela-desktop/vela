// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"

#include "batterysettings.h"
#include "clockformat.h"
#include "defaults.h"
#include "starticon.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

Config::Config(QObject* parent)
    : QObject(parent)
    , m_path(QSettings().fileName())
{
    // Whoever writes the file replaces it (QSaveFile): the directory is
    // watched too, and the file is watched again on every change.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(100);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        watch();
        reload();
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        m_pictureTouched |= path == m_watchedPicture;
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    watch();
    reload();
}

void Config::watch()
{
    const QFileInfo info(m_path);
    // On a new profile the directory isn't there yet: made here, so that
    // Settings' first write is seen (QSettings would make it anyway).
    QDir().mkpath(info.absolutePath());
    if (info.exists() && !m_watcher.files().contains(m_path)) {
        m_watcher.addPath(m_path);
    }
    if (info.dir().exists() && !m_watcher.directories().contains(info.absolutePath())) {
        m_watcher.addPath(info.absolutePath());
    }
}

QString Config::timeFormat() const
{
    return vela::clock::timeFormat(m_clock24);
}

QString Config::clockFormat() const
{
    return vela::clock::timeFormat(m_clock24, m_clockSeconds);
}

void Config::refreshStartIcon()
{
    QSettings settings;
    const QString source = vela::start::icon(settings).source;
    if (source != m_startIconSource) {
        m_startIconSource = source;
        emit startChanged();
    }
    watchPicture();
}

void Config::watchPicture()
{
    // Only a picture's file (a logo may be an icon theme's name); watched
    // again after an editor replaced it, which drops it from the watcher.
    const QString picture = m_startIconSource.startsWith(u'/') ? m_startIconSource : QString();
    if (!m_watchedPicture.isEmpty() && m_watchedPicture != picture) {
        m_watcher.removePath(m_watchedPicture);
    }
    m_watchedPicture = picture;
    if (!picture.isEmpty() && !m_watcher.files().contains(picture)) {
        m_watcher.addPath(picture);
    }
}

void Config::reload()
{
    QSettings settings;
    settings.sync(); // rereads what other processes wrote

    // VELA_WALLPAPER (for tests) takes precedence over the saved choice.
    QString wallpaper = qEnvironmentVariable("VELA_WALLPAPER");
    if (wallpaper.isEmpty()) {
        wallpaper = settings.value(QStringLiteral("appearance/wallpaper")).toString();
    }
    if (wallpaper.isEmpty() || (!wallpaper.startsWith(u':') && !QFileInfo::exists(wallpaper))) {
        wallpaper = vela::defaults::wallpaper();
    }
    if (wallpaper != m_wallpaper) {
        m_wallpaper = wallpaper;
        emit wallpaperChanged();
    }

    const QString alignment = settings.value(QStringLiteral("taskbar/alignment")).toString() == QLatin1String("left")
        ? QStringLiteral("left")
        : QStringLiteral("center");
    const bool endTask = settings.value(QStringLiteral("taskbar/endTask"), true).toBool();
    const bool taskView = settings.value(QStringLiteral("taskbar/taskView"), true).toBool();
    const bool allScreens = settings.value(QStringLiteral("taskbar/allScreens"), true).toBool();
    const bool showDesktopCorner = settings.value(QStringLiteral("taskbar/showDesktopCorner"), true).toBool();
    if (alignment != m_taskbarAlignment || endTask != m_endTask || taskView != m_taskView
        || allScreens != m_taskbarAllScreens || showDesktopCorner != m_showDesktopCorner) {
        m_taskbarAlignment = alignment;
        m_endTask = endTask;
        m_taskView = taskView;
        m_taskbarAllScreens = allScreens;
        m_showDesktopCorner = showDesktopCorner;
        emit taskbarChanged();
    }

    const bool clockDate = settings.value(QStringLiteral("taskbar/showDate"), true).toBool();
    const bool clock24 = vela::clock::hour24(settings);
    const bool clockSeconds = settings.value(QStringLiteral("taskbar/clockSeconds"), false).toBool();
    const bool notificationCount = settings.value(QStringLiteral("taskbar/notificationCount"), true).toBool();
    if (clockDate != m_clockDate || clock24 != m_clock24 || clockSeconds != m_clockSeconds
        || notificationCount != m_notificationCount) {
        m_clockDate = clockDate;
        m_clock24 = clock24;
        m_clockSeconds = clockSeconds;
        m_notificationCount = notificationCount;
        emit clockChanged();
    }

    const bool startPinned = settings.value(QStringLiteral("start/showPinned"), true).toBool();
    const QString startIconSource = vela::start::icon(settings).source;
    // The same picture, changed in place: a new image under the same name.
    const bool repainted = m_pictureTouched && !startIconSource.isEmpty() && startIconSource == m_startIconSource;
    m_pictureTouched = false;
    if (startPinned != m_startPinned || startIconSource != m_startIconSource || repainted) {
        m_startPinned = startPinned;
        m_startIconSource = startIconSource;
        m_startIconRevision += repainted ? 1 : 0;
        emit startChanged();
    }
    watchPicture();

    const QString primaryOutput = settings.value(QStringLiteral("display/primaryOutput")).toString();
    if (primaryOutput != m_primaryOutput) {
        m_primaryOutput = primaryOutput;
        emit primaryOutputChanged();
    }

    auto mode = [&](const char* key) {
        return settings.value(QLatin1String(key)).toString() == QLatin1String("light") ? QStringLiteral("light")
                                                                                       : QStringLiteral("dark");
    };
    const QString shellTheme = mode("appearance/shellTheme");
    const QString appTheme = mode("appearance/appTheme");
    if (shellTheme != m_shellTheme || appTheme != m_appTheme) {
        m_shellTheme = shellTheme;
        m_appTheme = appTheme;
        emit themeChanged();
    }

    const bool clipboard = settings.value(QStringLiteral("clipboard/history"), false).toBool();
    if (clipboard != m_clipboardHistory) {
        m_clipboardHistory = clipboard;
        emit clipboardChanged();
    }

    const bool desktopIcons = settings.value(QStringLiteral("desktop/showIcons"), true).toBool();
    if (desktopIcons != m_showDesktopIcons) {
        m_showDesktopIcons = desktopIcons;
        emit desktopIconsChanged(desktopIcons);
    }

    const bool dnd = settings.value(QStringLiteral("notifications/doNotDisturb"), false).toBool();
    if (dnd != m_doNotDisturb) {
        m_doNotDisturb = dnd;
        emit doNotDisturbChanged(dnd);
    }

    const bool silence = settings.value(QStringLiteral("notifications/silenceWhileSharing"), true).toBool();
    if (silence != m_silenceWhileSharing) {
        m_silenceWhileSharing = silence;
        emit silenceWhileSharingChanged(silence);
    }

    const bool batteryNotify = vela::battery::notify(settings);
    const int batteryLowLevel = vela::battery::lowLevel(settings);
    const bool deviceBatteryNotify = vela::battery::devices(settings);
    if (batteryNotify != m_batteryNotify || batteryLowLevel != m_batteryLowLevel
        || deviceBatteryNotify != m_deviceBatteryNotify) {
        m_batteryNotify = batteryNotify;
        m_batteryLowLevel = batteryLowLevel;
        m_deviceBatteryNotify = deviceBatteryNotify;
        emit batteryChanged();
    }
}
