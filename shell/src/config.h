// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

// The user's choices the shell follows live: they're in
// ~/.config/Vela/vela-shell.conf, written by the Settings app (or the shell
// itself) and reread here as soon as the file changes. From QML:
// Config.wallpaper, Config.taskbarAlignment... (light or dark, accent and
// icons reach QML through Vela.Controls' Style.)
class Config : public QObject {
    Q_OBJECT
    // The wallpaper: a chosen image, otherwise Vela's.
    Q_PROPERTY(QString wallpaper READ wallpaper NOTIFY wallpaperChanged)
    // "center" or "left", like "Taskbar alignment".
    Q_PROPERTY(QString taskbarAlignment READ taskbarAlignment NOTIFY taskbarChanged)
    // "End task" in the taskbar buttons' menu.
    Q_PROPERTY(bool endTask READ endTask NOTIFY taskbarChanged)
    // The Task View button on the taskbar.
    Q_PROPERTY(bool taskView READ taskView NOTIFY taskbarChanged)
    // The taskbar on all outputs (like Windows) or only on the main one.
    Q_PROPERTY(bool taskbarAllScreens READ taskbarAllScreens NOTIFY taskbarChanged)
    Q_PROPERTY(QString primaryOutput READ primaryOutput NOTIFY primaryOutputChanged)
    // Clickable right-hand taskbar corner that toggles the desktop.
    Q_PROPERTY(bool showDesktopCorner READ showDesktopCorner NOTIFY taskbarChanged)
    // The taskbar clock's second line, the date (taskbar/showDate).
    Q_PROPERTY(bool clockDate READ clockDate NOTIFY clockChanged)
    // 24-hour or 12-hour (clock/hours), and the seconds in the taskbar's
    // clock (taskbar/clockSeconds): Qt.formatTime() formats for the times
    // Vela shows (timeFormat) and for the taskbar's clock (clockFormat).
    Q_PROPERTY(QString timeFormat READ timeFormat NOTIFY clockChanged)
    Q_PROPERTY(QString clockFormat READ clockFormat NOTIFY clockChanged)
    Q_PROPERTY(bool clockSeconds READ clockSeconds NOTIFY clockChanged)
    // The number of notifications next to the clock
    // (taskbar/notificationCount); "Do not disturb"'s bell shows anyway.
    Q_PROPERTY(bool notificationCount READ notificationCount NOTIFY clockChanged)
    // What the Start button draws (shell/src/starticon.h): the
    // distribution's logo or a picture, "" for Vela's sail.
    Q_PROPERTY(QString startIconSource READ startIconSource NOTIFY startChanged)
    // The apps pinned to Start, above all apps (start/showPinned).
    Q_PROPERTY(bool startPinned READ startPinned NOTIFY startChanged)
    // Clipboard history (Win+V).
    Q_PROPERTY(bool clipboardHistory READ clipboardHistory NOTIFY clipboardChanged)
    // "Choose your mode": "dark" or "light", for the shell (taskbar, menus,
    // panels) and for apps (their windows and title bars).
    Q_PROPERTY(QString shellTheme READ shellTheme NOTIFY themeChanged)
    Q_PROPERTY(QString appTheme READ appTheme NOTIFY themeChanged)

public:
    explicit Config(QObject* parent = nullptr);

    QString wallpaper() const { return m_wallpaper; }
    QString taskbarAlignment() const { return m_taskbarAlignment; }
    bool endTask() const { return m_endTask; }
    bool taskView() const { return m_taskView; }
    bool taskbarAllScreens() const { return m_taskbarAllScreens; }
    QString primaryOutput() const { return m_primaryOutput; }
    bool showDesktopCorner() const { return m_showDesktopCorner; }
    bool clipboardHistory() const { return m_clipboardHistory; }
    bool startPinned() const { return m_startPinned; }
    QString startIconSource() const { return m_startIconSource; }
    bool clockDate() const { return m_clockDate; }
    bool clockSeconds() const { return m_clockSeconds; }
    bool notificationCount() const { return m_notificationCount; }
    QString timeFormat() const;
    QString clockFormat() const;
    bool doNotDisturb() const { return m_doNotDisturb; }
    QString shellTheme() const { return m_shellTheme; }
    QString appTheme() const { return m_appTheme; }
    // Desktop icons shown (desktop/showIcons): the desktop's own menu writes
    // it too (DesktopModel), Settings > Background here. Only for C++:
    // QML asks Desktop.showIcons.
    bool showDesktopIcons() const { return m_showDesktopIcons; }

signals:
    void wallpaperChanged();
    void taskbarChanged();
    void primaryOutputChanged();
    void doNotDisturbChanged(bool on);
    void themeChanged();
    void clipboardChanged();
    void startChanged();
    void clockChanged();
    void desktopIconsChanged(bool shown);

private:
    void reload();
    void watch();

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_path;
    QString m_wallpaper;
    QString m_taskbarAlignment;
    QString m_primaryOutput;
    bool m_endTask = true;
    bool m_taskView = true;
    bool m_taskbarAllScreens = true;
    bool m_showDesktopCorner = true;
    bool m_clipboardHistory = false;
    bool m_startPinned = true;
    QString m_startIconSource;
    bool m_clockDate = true;
    bool m_clock24 = true;
    bool m_clockSeconds = false;
    bool m_notificationCount = true;
    bool m_doNotDisturb = false;
    bool m_showDesktopIcons = true;
    QString m_shellTheme = QStringLiteral("dark");
    QString m_appTheme = QStringLiteral("dark");
};
