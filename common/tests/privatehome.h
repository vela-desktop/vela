// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A home of its own for a C++ test of Vela's settings (shell/tests/
// config_test.cpp, settings/tests/preferences_test.cpp): the XDG and
// runtime directories in a temporary directory, so that nothing the test
// writes reaches the user's configuration and no compositor answers it.
// isolate() before the QGuiApplication; the directory goes at exit.

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <utility>

namespace vela::test {

inline QTemporaryDir& home()
{
    static QTemporaryDir dir;
    return dir;
}

// ~/.config/Vela/vela-shell.conf, the shell's choices.
inline QString confPath()
{
    return home().filePath(QStringLiteral("config/Vela/vela-shell.conf"));
}

// A file under home(), its directories made.
inline bool writeFile(const QString& relative, const QByteArray& text)
{
    const QString path = home().filePath(relative);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(text) == text.size();
}

// A real picture (a PNG of one color), as Qt reads it.
inline bool paint(const QString& path, const QColor& color)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage image(16, 16, QImage::Format_ARGB32);
    image.fill(color);
    return image.save(path, "PNG");
}

// HOME, the XDG directories (the system's data too) and the runtime
// directory under home(), with a Wayland display no compositor serves.
inline bool isolate()
{
    if (!home().isValid()) {
        return false;
    }
    for (const auto& [name, leaf] : { std::pair { "HOME", "home" }, std::pair { "XDG_CONFIG_HOME", "config" },
             std::pair { "XDG_DATA_HOME", "data" }, std::pair { "XDG_CACHE_HOME", "cache" },
             std::pair { "XDG_STATE_HOME", "state" }, std::pair { "XDG_RUNTIME_DIR", "runtime" },
             std::pair { "XDG_DATA_DIRS", "system" } }) {
        const QString path = home().filePath(QLatin1String(leaf));
        QDir().mkpath(path);
        qputenv(name, path.toUtf8());
    }
    QFile::setPermissions(home().filePath(QStringLiteral("runtime")),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("WAYLAND_DISPLAY", "vela-private-test");
    qputenv("VELA_OS_RELEASE", home().filePath(QStringLiteral("os-release")).toUtf8());
    qunsetenv("VELA_WALLPAPER");
    return true;
}

// A logo only in an icon theme ("themed-logo" in home()/themes/test, a PNG:
// whether Qt reads SVG icons depends on its plugins), and an os-release
// naming it, whose path this returns.
inline QString themedOsRelease()
{
    writeFile(QStringLiteral("themes/test/index.theme"),
        "[Icon Theme]\nName=Test\nDirectories=apps\n\n[apps]\nSize=48\nType=Fixed\n");
    paint(home().filePath(QStringLiteral("themes/test/apps/themed-logo.png")), Qt::green);
    writeFile(QStringLiteral("themed-os-release"), "NAME=Themed\nLOGO=themed-logo\n");
    return home().filePath(QStringLiteral("themed-os-release"));
}

} // namespace vela::test
