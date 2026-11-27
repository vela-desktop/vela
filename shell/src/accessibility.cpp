// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accessibility.h"

#include "compositorsocket.h"
#include "shellcontroller.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

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
    update(compositor::ask("accessibility", 200));
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

bool Accessibility::sendCaffeineCommand(const QByteArray& command)
{
    const QJsonObject response = QJsonDocument::fromJson(compositor::ask("caffeine-set " + command, 300)).object();
    const QJsonValue state = response.value(QStringLiteral("state"));
    const QJsonValue ok = response.value(QStringLiteral("ok"));
    if (state.isObject() && ok.isBool()) {
        // The compositor applied or rejected the request before generating
        // this state. Never display a speculative "off".
        update(QJsonDocument(state.toObject()).toJson(QJsonDocument::Compact));
        if (!ok.toBool()) {
            qWarning() << "vela-shell: compositor rejected caffeine command:" << command;
        }
        return ok.toBool();
    }
    qWarning() << "vela-shell: caffeine state unconfirmed; keeping last known state:" << command;
    // The compositor may have applied a request despite a lost reply.
    QTimer::singleShot(250, this, &Accessibility::query);
    QTimer::singleShot(1000, this, &Accessibility::query);
    return false;
}

void Accessibility::setCaffeine(bool on)
{
    sendCaffeineCommand(on ? "on" : "off");
}

void Accessibility::setCaffeineMinutes(double minutes)
{
    if (!std::isfinite(minutes) || minutes <= 0 || minutes > 1440.0) {
        return;
    }
    sendCaffeineCommand(QStringLiteral("on %1").arg(minutes).toUtf8());
}
