// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QSettings>

namespace vela::taskbar {

inline bool enabled(const QSettings& settings, const QString& key)
{
    const QString value = settings.value(key).toString().trimmed().toLower();
    return value != QLatin1String("false") && value != QLatin1String("0");
}

}
