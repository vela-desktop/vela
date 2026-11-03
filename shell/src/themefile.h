// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-FileCopyrightText: 2026 lowell
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtLogging>

// Theme files (theme=<name> in vela.conf, themes/<name>.conf): the colors and
// corner radii that Theme.qml takes over Vela's own. `key=value` lines and
// `#` comments; a key is a Theme.qml token, "dark.<token>" or "light.<token>"
// for one mode only. A value is a color (#RRGGBB, #AARRGGBB) or a number.
//
// The user's themes (~/.config/vela/themes/) win over the installed ones
// (the data directories: a package, or a nested run's XDG_DATA_DIRS).
namespace vela::themes {

inline const QString& subdirectory()
{
    static const QString dir = QStringLiteral("vela/themes");
    return dir;
}

// A name is a file name without ".conf": nothing that leads elsewhere.
inline bool validName(const QString& name)
{
    return !name.isEmpty() && !name.contains(u'/') && !name.startsWith(u'.');
}

// theme= of a vela.conf; the last one if there are several (like the compositor).
inline QString configuredName(const QString& velaConf)
{
    QFile file(velaConf);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QString name;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QLatin1String("theme="))) {
            name = line.mid(6).trimmed();
        }
    }
    return name;
}

// The theme's file, or empty when the name is not valid or not installed.
inline QString locate(const QString& name)
{
    if (!validName(name)) {
        return {};
    }
    const QString relative = subdirectory() + u'/' + name + QStringLiteral(".conf");
    const QString user = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, relative);
    return user.isEmpty() ? QStandardPaths::locate(QStandardPaths::GenericDataLocation, relative) : user;
}

// The values of a theme: a number as double, a color as QColor. A line that
// is neither is skipped with a warning (`origin` names the file in it).
inline QVariantMap parse(QIODevice& in, const QString& origin = QString())
{
    QVariantMap map;
    int number = 0;
    while (!in.atEnd()) {
        ++number;
        const QString line = QString::fromUtf8(in.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(u'#')) {
            continue;
        }
        const qsizetype equals = line.indexOf(u'=');
        const QString key = equals > 0 ? line.left(equals).trimmed() : QString();
        const QString value = equals > 0 ? line.mid(equals + 1).trimmed() : QString();
        bool isNumber = false;
        const double d = value.toDouble(&isNumber);
        if (key.isEmpty() || value.isEmpty()) {
            qWarning("theme %s:%d: not a key=value line", qPrintable(origin), number);
        } else if (isNumber) {
            map.insert(key, d);
        } else if (value.startsWith(u'#') && QColor::isValidColorName(value)) {
            map.insert(key, QColor::fromString(value));
        } else {
            qWarning("theme %s:%d: %s is neither a color nor a number", qPrintable(origin), number, qPrintable(key));
        }
    }
    return map;
}

// A token's value as Theme.qml's themed() finds it: "dark.<key>" or
// "light.<key>" for the mode, else "<key>"; invalid when the theme lacks it.
inline QVariant themed(const QVariantMap& theme, const QString& key, bool light)
{
    const QString modeKey = (light ? QStringLiteral("light.") : QStringLiteral("dark.")) + key;
    return theme.contains(modeKey) ? theme.value(modeKey) : theme.value(key);
}

inline QVariantMap load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return parse(file, path);
}

// The installed themes, by name: { name, text }. The text is the name the
// first line gives ("# Rosé Pine: ..."), else the file name.
inline QVariantList available()
{
    QStringList roots = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, subdirectory(), QStandardPaths::LocateDirectory);
    roots.prepend(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u'/' + subdirectory());
    QMap<QString, QString> files; // sorted by name; the first root wins
    for (const QString& root : std::as_const(roots)) {
        for (const QString& file : QDir(root).entryList({ QStringLiteral("*.conf") }, QDir::Files)) {
            if (!files.contains(file.chopped(5))) {
                files.insert(file.chopped(5), root + u'/' + file);
            }
        }
    }
    QVariantList list;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QString text = it.key();
        QFile file(it.value());
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString first = QString::fromUtf8(file.readLine()).trimmed();
            const qsizetype colon = first.indexOf(u':');
            if (first.startsWith(u'#') && colon > 1) {
                text = first.mid(1, colon - 1).trimmed();
            }
        }
        list.append(QVariantMap { { QStringLiteral("name"), it.key() }, { QStringLiteral("text"), text } });
    }
    return list;
}

} // namespace vela::themes
