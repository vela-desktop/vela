// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The Mica color of Vela's windows: the same computation the compositor uses
// for the title bar (compositor/src/decoration.c, mica_color), so the content
// of Vela's apps (Settings, Explorer) continues the bar seamlessly. The tint
// is the wallpaper's average color: the shell computes it and also leaves it
// in ~/.cache/vela/wallpaper-tint, for apps to read quickly at startup.

#include <QColor>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <algorithm>

inline QString wallpaperTintCachePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/vela/wallpaper-tint");
}

inline void saveWallpaperTint(const QColor& tint)
{
    const QString path = wallpaperTintCachePath();
    QDir().mkpath(path.section(u'/', 0, -2));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QByteArray::number(tint.red()) + ' ' + QByteArray::number(tint.green()) + ' '
            + QByteArray::number(tint.blue()) + '\n');
    }
}

// Invalid if the shell hasn't written it yet.
inline QColor cachedWallpaperTint()
{
    QFile file(wallpaperTintCachePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QList<QByteArray> parts = file.readAll().trimmed().split(' ');
    if (parts.size() != 3) {
        return {};
    }
    return QColor(parts[0].toInt(), parts[1].toInt(), parts[2].toInt());
}

// The wallpaper's source color (vela::color::sourceFromPixels: its most
// suitable color for a tonal palette, not its average), which the shell
// leaves next to the tint in ~/.cache/vela/wallpaper-color as "#rrggbb".
// The palette follows the wallpaper when the user or the theme asks
// (accentsource.h). An empty file: the wallpaper has no such color (a
// grayscale one), the chosen accent stays.
inline QString wallpaperColorCachePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/vela/wallpaper-color");
}

inline void saveWallpaperColor(const QColor& color)
{
    const QString path = wallpaperColorCachePath();
    QDir().mkpath(path.section(u'/', 0, -2));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(color.isValid() ? color.name().toLatin1() + '\n' : QByteArray("\n"));
    }
}

// Invalid if the shell hasn't written it yet, or the wallpaper has none.
inline QColor cachedWallpaperColor()
{
    QFile file(wallpaperColorCachePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray line = file.readAll().trimmed();
    if (line.size() != 7 || !line.startsWith('#')) {
        return {};
    }
    return QColor::fromString(QLatin1StringView(line));
}

// Dark: active base #202020 30% toward the tint, inactive #2b2b2b 45%. Light:
// #f3f3f3 and #f9f9f9, with the tint brought into the light band. Like
// mica_color in compositor/src/decoration.c.
inline QColor micaFromTint(const QColor& tint, bool active, bool light = false)
{
    const float base = light ? (active ? 0.953f : 0.976f) : (active ? 0.125f : 0.169f);
    if (!tint.isValid()) {
        return QColor::fromRgbF(base, base, base);
    }
    const float weight = active ? 0.30f : 0.45f;
    const float t[3] = { float(tint.redF()), float(tint.greenF()), float(tint.blueF()) };
    const float luma = 0.2126f * t[0] + 0.7152f * t[1] + 0.0722f * t[2];
    float out[3];
    for (int i = 0; i < 3; ++i) {
        const float desaturated = luma + (t[i] - luma) * 0.5f;
        const float safe = light
            ? std::clamp(1.0f - (1.0f - desaturated) * (0.10f / std::max(1.0f - luma, 0.02f)), 0.82f, 1.0f)
            : std::clamp(desaturated * (0.16f / std::max(luma, 0.02f)), 0.0f, 0.32f);
        out[i] = base + (safe - base) * weight;
    }
    return QColor::fromRgbF(out[0], out[1], out[2]);
}
