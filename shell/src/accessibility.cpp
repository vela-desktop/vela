// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accessibility.h"

#include "shellcontroller.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStandardPaths>

#include <cmath>

Accessibility::Accessibility(QObject* parent)
    : QObject(parent)
{
    m_caffeineTicker.setInterval(500);
    connect(&m_caffeineTicker, &QTimer::timeout, this, [this] {
        const int remaining = qMax(0, m_caffeineStartSecs - (int)(m_caffeineElapsed.elapsed() / 1000));
        if (remaining != m_caffeineSecs) {
            m_caffeineSecs = remaining;
            emit changed();
        }
        if (remaining == 0) {
            m_caffeineTicker.stop(); // compositor will confirm expiry
        }
    });
}

void Accessibility::resetCaffeineCountdown()
{
    m_caffeineTicker.stop();
    if (m_caffeine && m_caffeineSecs > 0) {
        m_caffeineStartSecs = m_caffeineSecs;
        m_caffeineElapsed.start();
        m_caffeineTicker.start();
    }
}

void Accessibility::query()
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    QLocalSocket socket;
    socket.connectToServer(runtimeDir + QStringLiteral("/vela-") + display + QStringLiteral(".sock"));
    if (!socket.waitForConnected(200)) {
        return;
    }
    socket.write("accessibility\n");
    if (!socket.waitForBytesWritten(200)) {
        return;
    }
    QByteArray reply;
    while (!reply.contains('\n') && socket.waitForReadyRead(200)) {
        reply += socket.readAll();
    }
    update(reply.trimmed());
}

void Accessibility::update(const QByteArray& json)
{
    const QJsonObject state = QJsonDocument::fromJson(json).object();
    if (state.isEmpty()) {
        return;
    }
    m_nightLight = state[QStringLiteral("nightLight")].toBool();
    m_colorFilter = state[QStringLiteral("colorFilter")].toBool();
    m_magnifier = state[QStringLiteral("magnifier")].toBool();
    m_stickyKeys = state[QStringLiteral("stickyKeys")].toBool();
    m_caffeine = state[QStringLiteral("caffeine")].toBool();
    m_caffeineSecs = state[QStringLiteral("caffeineSecs")].toInt();
    m_caffeineDurationSecs = state[QStringLiteral("caffeineDurationSecs")].toInt();
    resetCaffeineCountdown();
    emit changed();
}

// Updated at once (the tile responds to the click), then the compositor
// confirms.
void Accessibility::setNightLight(bool on)
{
    m_nightLight = on;
    emit changed();
    ShellController::sendToCompositor(on ? "night-light on" : "night-light off");
}

void Accessibility::setColorFilter(bool on)
{
    m_colorFilter = on;
    emit changed();
    ShellController::sendToCompositor(on ? "color-filter on" : "color-filter off");
}

void Accessibility::setMagnifier(bool on)
{
    m_magnifier = on;
    emit changed();
    ShellController::sendToCompositor(on ? "magnifier on" : "magnifier off");
}

void Accessibility::setStickyKeys(bool on)
{
    m_stickyKeys = on;
    emit changed();
    ShellController::sendToCompositor(on ? "sticky-keys on" : "sticky-keys off");
}

void Accessibility::setCaffeine(bool on)
{
    m_caffeine = on;
    m_caffeineSecs = on ? -1 : 0;
    m_caffeineDurationSecs = 0;
    resetCaffeineCountdown();
    emit changed();
    ShellController::sendToCompositor(on ? "caffeine on" : "caffeine off");
}

void Accessibility::setCaffeineMinutes(double minutes)
{
    if (!std::isfinite(minutes) || minutes <= 0 || minutes > 1440.0) {
        return;
    }
    m_caffeine = true;
    m_caffeineSecs = (int)std::ceil(minutes * 60.0);
    m_caffeineDurationSecs = m_caffeineSecs;
    resetCaffeineCountdown();
    emit changed();
    ShellController::sendToCompositor(QStringLiteral("caffeine on %1").arg(minutes).toUtf8());
}
