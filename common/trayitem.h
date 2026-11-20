// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QDBusArgument>
#include <QString>
#include <QVariant>
#include <QVariantMap>

// A StatusNotifierItem's names, the same in the shell's notification area
// (shell/src/tray.cpp) and in Settings' list of its icons
// (settings/src/trayicons.cpp): taskbar/trayShown holds these keys
// (docs/taskbar.md §8).
namespace vela::tray {

// The ToolTip property's title: (sa(iiay)ss).
inline QString tooltipTitle(const QVariant& value)
{
    if (!value.canConvert<QDBusArgument>()) {
        return {};
    }
    const QDBusArgument argument = value.value<QDBusArgument>();
    QString iconName;
    QString title;
    QString description;
    argument.beginStructure();
    argument >> iconName;
    argument.beginArray();
    while (!argument.atEnd()) {
        int width = 0;
        int height = 0;
        QByteArray bytes;
        argument.beginStructure();
        argument >> width >> height >> bytes;
        argument.endStructure();
    }
    argument.endArray();
    argument >> title >> description;
    argument.endStructure();
    return title;
}

// What the user's choice is saved by: the Id, stable across runs
// ("discord", "steam"); a few apps leave it empty, and their Title, else the
// tooltip's title, is the next best thing. Trimmed, as the saved list is.
inline QString key(const QVariantMap& properties)
{
    for (const QString& name : { QStringLiteral("Id"), QStringLiteral("Title") }) {
        const QString value = properties.value(name).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return tooltipTitle(properties.value(QStringLiteral("ToolTip"))).trimmed();
}

// Whether the key comes from the Id, and so may replace an earlier one: a
// key taken from a title stays the item's while it runs, so a title that
// counts unread messages does not lose the user's choice.
inline bool hasId(const QVariantMap& properties)
{
    return !properties.value(QStringLiteral("Id")).toString().trimmed().isEmpty();
}

// The name shown: the tooltip's title, else the Title, else the Id.
inline QString title(const QVariantMap& properties)
{
    QString title = tooltipTitle(properties.value(QStringLiteral("ToolTip")));
    if (title.isEmpty()) {
        title = properties.value(QStringLiteral("Title")).toString();
    }
    return title.isEmpty() ? properties.value(QStringLiteral("Id")).toString() : title;
}

}
