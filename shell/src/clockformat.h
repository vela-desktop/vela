// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QSettings>
#include <QString>

// How Vela writes the time: 24-hour ("13:45") or 12-hour ("1:45 PM"), as
// chosen in Settings > Taskbar (clock/hours= in vela-shell.conf, 24 unless
// it says 12). The taskbar, the notification center, the lock screen and
// Settings' Date & time page all follow it; only the taskbar's clock can
// show the seconds too.
namespace vela::clock {

inline bool hour24(const QSettings& settings)
{
    return settings.value(QStringLiteral("clock/hours")).toString() != QLatin1String("12");
}

// A QTime/Qt.formatTime() format. "AP" is the locale's AM or PM.
inline QString timeFormat(bool hour24, bool seconds = false)
{
    if (hour24) {
        return seconds ? QStringLiteral("HH:mm:ss") : QStringLiteral("HH:mm");
    }
    return seconds ? QStringLiteral("h:mm:ss AP") : QStringLiteral("h:mm AP");
}

} // namespace vela::clock
