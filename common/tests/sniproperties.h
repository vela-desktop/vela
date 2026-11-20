// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The structured properties of an org.kde.StatusNotifierItem that fake items
// in the tests publish: the ToolTip, (icon name, pictures, title, text), a
// picture being (width, height, ARGB bytes). Only written: the code under
// test is the one that reads them. Register both with qDBusRegisterMetaType
// before use.

#include <QByteArray>
#include <QDBusArgument>
#include <QMetaType>
#include <QString>

struct Picture {
    int width = 0;
    int height = 0;
    QByteArray bytes;
};
Q_DECLARE_METATYPE(Picture)

inline QDBusArgument& operator<<(QDBusArgument& argument, const Picture& picture)
{
    argument.beginStructure();
    argument << picture.width << picture.height << picture.bytes;
    argument.endStructure();
    return argument;
}
inline const QDBusArgument& operator>>(const QDBusArgument& argument, Picture&)
{
    return argument;
}

struct ToolTip {
    QString title;
};
Q_DECLARE_METATYPE(ToolTip)

inline QDBusArgument& operator<<(QDBusArgument& argument, const ToolTip& tip)
{
    argument.beginStructure();
    argument << QString();
    argument.beginArray(QMetaType::fromType<Picture>());
    argument.endArray();
    argument << tip.title << QString();
    argument.endStructure();
    return argument;
}
inline const QDBusArgument& operator>>(const QDBusArgument& argument, ToolTip&)
{
    return argument;
}
