// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QSettings>

namespace vela::taskbar {

// taskbar/position: the edge the taskbar sits on (docs/taskbar.md §11);
// anything else is the default, the bottom.
inline QString position(const QSettings& settings)
{
    const QString value = settings.value(QStringLiteral("taskbar/position")).toString().trimmed().toLower();
    return value == QLatin1String("top") || value == QLatin1String("left") || value == QLatin1String("right")
        ? value
        : QStringLiteral("bottom");
}

inline bool enabled(const QSettings& settings, const QString& key)
{
    const QString value = settings.value(key).toString().trimmed().toLower();
    return value != QLatin1String("false") && value != QLatin1String("0");
}

}
