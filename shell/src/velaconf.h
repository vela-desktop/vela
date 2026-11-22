// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// vela.conf's keys for the programs that only read a few of them (the
// shell's appearance, vela-lock), as the compositor reads the file
// (compositor/src/config.c): key=value lines, '#' a comment, the last line
// of a key wins.

#include <QFile>
#include <QHash>
#include <QString>
#include <QtGlobal>

namespace vela::conf {

inline QHash<QString, QString> read(const QString& path)
{
    QHash<QString, QString> keys;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return keys;
    }
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        const qsizetype eq = line.indexOf(u'=');
        if (line.startsWith(u'#') || eq <= 0) {
            continue;
        }
        keys.insert(line.left(eq), line.mid(eq + 1).trimmed());
    }
    return keys;
}

// yes, 1 or true; empty or missing is `fallback` (vela_config_flag()).
inline bool flag(const QHash<QString, QString>& keys, const QString& key, bool fallback)
{
    const QString value = keys.value(key);
    if (value.isEmpty()) {
        return fallback;
    }
    return value == QLatin1String("yes") || value == QLatin1String("1") || value == QLatin1String("true");
}

// animation-effects and animation-duration (the compositor reads them too):
// on, at Vela's durations, unless the file says otherwise.
struct Motion {
    bool effects = true;
    qreal duration = 1.0;
};

inline Motion motion(const QHash<QString, QString>& keys)
{
    Motion motion;
    const QString effects = keys.value(QStringLiteral("animation-effects"));
    motion.effects =
        effects != QLatin1String("no") && effects != QLatin1String("0") && effects != QLatin1String("false");
    bool ok = false;
    const int percent = keys.value(QStringLiteral("animation-duration")).toInt(&ok);
    motion.duration = ok ? qBound(50, percent, 300) / 100.0 : 1.0;
    return motion;
}

} // namespace vela::conf
