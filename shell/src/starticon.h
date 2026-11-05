// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

// What the Start button shows, as chosen in Settings > Start (start/icon=
// in vela-shell.conf): Vela's sail ("vela", the default), the
// distribution's logo ("distribution": os-release's LOGO=) or a picture
// ("picture": start/iconPicture=, a file). The shell and Settings ask the
// same questions here, so what Settings says is what the taskbar draws.
namespace vela::start {

// The distribution, from os-release (/etc, else /usr/lib; VELA_OS_RELEASE
// for tests): its name and its logo, an icon theme's name or a file's path
// for IconProvider, "" when the system has no such icon.
struct Distribution {
    QString name;
    QString logo;
};

inline QString osReleaseValue(const QByteArray& line)
{
    QByteArray value = line.mid(line.indexOf('=') + 1).trimmed();
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
        value = value.mid(1, value.size() - 2);
    }
    return QString::fromUtf8(value).replace(QStringLiteral("\\\""), QStringLiteral("\""));
}

// LOGO= names an icon (the freedesktop icon spec): a loose file in a data
// directory's icons/ or pixmaps/, as some distributions install it
// (CachyOS: /usr/share/icons/cachyos.svg), else the icon theme's. The file
// comes first, as its path: whether an icon engine finds loose files by
// name differs (Qt's does, KDE's not always).
inline QString resolveLogo(const QString& name)
{
    if (name.isEmpty() || name.contains(u'/')) {
        return {};
    }
    QStringList dirs;
    for (const QString& data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        dirs << data + QStringLiteral("/icons") << data + QStringLiteral("/pixmaps");
    }
    for (const QString& dir : dirs) {
        for (const char* extension : { ".svg", ".png", ".xpm" }) {
            const QString path = dir + u'/' + name + QLatin1String(extension);
            if (QFileInfo(path).isFile()) {
                return path;
            }
        }
    }
    return QIcon::hasThemeIcon(name) ? name : QString();
}

inline Distribution distribution()
{
    QStringList paths { QStringLiteral("/etc/os-release"), QStringLiteral("/usr/lib/os-release") };
    if (qEnvironmentVariableIsSet("VELA_OS_RELEASE")) {
        paths = { qEnvironmentVariable("VELA_OS_RELEASE") };
    }
    for (const QString& path : paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        QString name, prettyName, logo;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (line.startsWith("NAME=")) {
                name = osReleaseValue(line);
            } else if (line.startsWith("PRETTY_NAME=")) {
                prettyName = osReleaseValue(line);
            } else if (line.startsWith("LOGO=")) {
                logo = osReleaseValue(line);
            }
        }
        return { prettyName.isEmpty() ? name : prettyName, resolveLogo(logo) };
    }
    return {};
}

// What the button draws: its kind and, but for Vela's sail, its source.
// A choice that can't be shown (no logo, the picture gone) is Vela's sail.
struct Icon {
    QString kind = QStringLiteral("vela");
    QString source;
};

inline Icon icon(const QSettings& settings)
{
    const QString kind = settings.value(QStringLiteral("start/icon")).toString();
    if (kind == QLatin1String("distribution")) {
        const QString logo = distribution().logo;
        if (!logo.isEmpty()) {
            return { kind, logo };
        }
    } else if (kind == QLatin1String("picture")) {
        const QString picture = settings.value(QStringLiteral("start/iconPicture")).toString();
        if (picture.startsWith(u'/') && QFileInfo(picture).isFile()) {
            return { kind, picture };
        }
    }
    return {};
}

} // namespace vela::start
