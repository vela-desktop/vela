// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screenwindows.h"
#include "../../common/primaryoutput.h"

#include <LayerShellQt/Window>

#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QtDebug>

namespace {

bool realScreen(const QScreen* screen)
{
    return screen && !screen->name().isEmpty(); // no name: Qt's placeholder
}

} // namespace

ScreenWindows::ScreenWindows(QQmlEngine* engine, const char* component, Setup setup, QObject* parent)
    : QObject(parent)
    , m_component(engine, "Vela.Shell", component)
    , m_name(QString::fromLatin1(component))
    , m_setup(std::move(setup))
{
    m_syncTimer.setSingleShot(true);
    m_syncTimer.setInterval(100); // outputs arrive one at a time
    connect(&m_syncTimer, &QTimer::timeout, this, &ScreenWindows::sync);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] {
        m_retries = 0;
        scheduleSync();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* screen) {
        remove(screen);
        scheduleSync();
    });
    connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, &ScreenWindows::scheduleSync);
}

bool ScreenWindows::start()
{
    if (m_component.isError()) {
        qCritical() << "vela-shell:" << m_name << m_component.errors();
        return false;
    }
    sync();
    return true;
}

void ScreenWindows::setPrimaryOnly(bool on)
{
    if (on != m_primaryOnly) {
        m_primaryOnly = on;
        scheduleSync();
    }
}

void ScreenWindows::setPreferredPrimary(const QString& name)
{
    if (name != m_preferredPrimary) {
        m_preferredPrimary = name;
        scheduleSync();
    }
}

void ScreenWindows::scheduleSync()
{
    m_syncTimer.start();
}

void ScreenWindows::remove(QScreen* screen)
{
    if (QPointer<QQuickWindow> window = m_windows.take(screen)) {
        window->deleteLater();
    }
}

void ScreenWindows::sync()
{
    QScreen* primary = vela::primaryOutputScreen(m_preferredPrimary);
    for (QScreen* screen : QGuiApplication::screens()) {
        if (!realScreen(screen)) {
            continue;
        }
        if (m_primaryOnly && screen != primary) {
            remove(screen);
            continue;
        }
        QPointer<QQuickWindow>& window = m_windows[screen];
        if (!window) {
            QObject* object = m_component.create();
            window = qobject_cast<QQuickWindow*>(object);
            if (!window) {
                delete object;
                m_windows.remove(screen);
                continue;
            }
            static_cast<QObject*>(window)->setParent(this); // ownership, not parent window
            qInfo("vela-shell: %s on %s%s", qPrintable(m_name), qPrintable(screen->name()),
                screen == primary ? " (principale)" : "");
            window->setScreen(screen);
            m_setup(window, screen);
            if (m_name == QLatin1String("Wallpaper")) {
                // QScreen::devicePixelRatio rounds 125% to 2, unlike the
                // actual fractional render-target DPR on QQuickWindow.
                // Keep the QML selection rectangle aligned to physical
                // pixels even after a Wayland output-scale change.
                const QPointer<QQuickWindow> wallpaper = window;
                auto publishScale = [wallpaper] {
                    if (wallpaper) {
                        wallpaper->setProperty("renderScale", wallpaper->effectiveDevicePixelRatio());
                    }
                };
                connect(screen, &QScreen::logicalDotsPerInchChanged, window, publishScale);
                connect(window, &QWindow::screenChanged, window, publishScale);
                // Fractional DPR may only settle after the initial Wayland
                // configure, when the mapped layer acquires its real size.
                connect(window, &QWindow::widthChanged, window, publishScale);
                connect(window, &QWindow::heightChanged, window, publishScale);
                QTimer::singleShot(0, window, publishScale);
            }
            // Closed by the compositor: put back, but without insisting if it
            // keeps closing it.
            connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
                if (!visible && m_retries < 5) {
                    ++m_retries;
                    scheduleSync();
                }
            });
        }
        window->setProperty("primary", screen == primary);
        if (!window->isVisible()) {
            window->setScreen(screen);
            window->show();
        }
        if (m_name == QLatin1String("Wallpaper")) {
            window->setProperty("renderScale", window->effectiveDevicePixelRatio());
        }
    }
}
