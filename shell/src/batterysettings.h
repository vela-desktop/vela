// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QSettings>

// The battery notifications' settings in vela-shell.conf
// (docs/battery-notifications.md §6), read the same way by the shell and by
// Settings: battery/notify (the PC's battery, on), battery/lowLevel (its
// low warning, in percent) and battery/devices (the peripherals', on).
namespace vela::battery {

inline constexpr int defaultLowLevel = 20;
// What Settings offers; the file may hold any whole number in the range.
inline constexpr int lowLevels[] = { 10, 15, 20, 25, 30, 40, 50 };
inline constexpr int minimumLowLevel = 5;
inline constexpr int maximumLowLevel = 95;

inline bool notify(const QSettings& settings)
{
    return settings.value(QStringLiteral("battery/notify"), true).toBool();
}

// Outside 5-95, or not a number: 20.
inline int lowLevel(const QSettings& settings)
{
    bool ok = false;
    const int level = settings.value(QStringLiteral("battery/lowLevel"), defaultLowLevel).toInt(&ok);
    return ok && level >= minimumLowLevel && level <= maximumLowLevel ? level : defaultLowLevel;
}

inline bool devices(const QSettings& settings)
{
    return settings.value(QStringLiteral("battery/devices"), true).toBool();
}

} // namespace vela::battery
