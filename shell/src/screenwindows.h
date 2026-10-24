// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQmlComponent>
#include <QTimer>

#include <functional>

class QQmlEngine;
class QQuickWindow;
class QScreen;

// One shell window per output, born and gone with the output: the wallpaper
// (Wallpaper.qml) and the taskbar (Taskbar.qml), like in Windows. Each window
// has the `primary` property (the main output: desktop icons and the
// notification area go there). If the compositor closes it (output turned off
// and on, VT switch) it's put back.
class ScreenWindows : public QObject {
    Q_OBJECT

public:
    using Setup = std::function<void(QQuickWindow*, QScreen*)>;
    ScreenWindows(QQmlEngine* engine, const char* component, Setup setup, QObject* parent = nullptr);
    bool start(); // false if the QML doesn't load

    // Only on the main output (the taskbar, if the user wants it there alone).
    void setPrimaryOnly(bool on);
    void setPreferredPrimary(const QString& name);

private:
    void sync();
    void scheduleSync();
    void remove(QScreen* screen);

    QQmlComponent m_component;
    QString m_name;
    Setup m_setup;
    bool m_primaryOnly = false;
    QString m_preferredPrimary;
    QHash<QScreen*, QPointer<QQuickWindow>> m_windows;
    QTimer m_syncTimer;
    int m_retries = 0; // reopenings without a new output
};
