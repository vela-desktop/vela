// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QGuiApplication>
#include <QScreen>
#include <QString>
#include <QStringList>

// Keep a preferred output across hotplug, without saving a fallback.
namespace vela {
inline QString primaryOutputName(const QString& preferred, const QStringList& connected, const QString& qtPrimary)
{
    if (!preferred.isEmpty() && connected.contains(preferred)) return preferred;
    if (!qtPrimary.isEmpty() && connected.contains(qtPrimary)) return qtPrimary;
    return connected.value(0);
}
inline QScreen* primaryOutputScreen(const QString& preferred)
{
    QStringList connected;
    for (QScreen* s : QGuiApplication::screens())
        if (s && !s->name().isEmpty()) connected.append(s->name());
    const QScreen* qt = QGuiApplication::primaryScreen();
    const QString name = primaryOutputName(preferred, connected, qt ? qt->name() : QString());
    for (QScreen* s : QGuiApplication::screens())
        if (s && s->name() == name) return s;
    return nullptr;
}
} // namespace vela
