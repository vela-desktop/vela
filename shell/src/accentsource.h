// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Where the accent comes from: the one rule for Style (appearance.cpp) and
// the lock screen, so the two never disagree.
//
//   1. the theme's `accent` for the mode wins over everything (as Theme.qml
//      has always done);
//   2. else the wallpaper's source color (mica.h's wallpaper-color), when
//      the user asks for it (appearance/accentSource=wallpaper in
//      vela-shell.conf), or leaves it to the theme
//      (appearance/accentSource=theme, the default) and the theme asks for
//      it (color-source=wallpaper); only if the wallpaper has one;
//   3. else the accent chosen in Settings > Colors (appearance/accent), or
//      Vela's.
//
// The tonal palette (Style.tonal) starts from the theme's `primary` if it
// has one, else from that accent: a theme that sets only `primary` gets
// containers that match it.

#include "themefile.h"

#include <QColor>
#include <QString>
#include <QVariantMap>

namespace vela::accent {

// appearance/accentSource: "theme", "wallpaper" or "chosen".
enum class Source { Theme, Wallpaper, Chosen };

inline Source sourceFromString(const QString& value)
{
    if (value == QLatin1String("wallpaper")) {
        return Source::Wallpaper;
    }
    if (value == QLatin1String("chosen")) {
        return Source::Chosen;
    }
    return Source::Theme;
}

// Which step of the rule gave the accent: what Settings explains.
enum class Origin { Theme, Wallpaper, Chosen };

struct Resolved {
    QColor color;
    Origin origin = Origin::Chosen;
};

// Steps 2 and 3: the user's accent, the one Style.accent gives and Theme.qml
// takes when the theme has no accent of its own.
inline Resolved userAccent(const QVariantMap& theme, bool light, Source source, const QColor& chosen,
    const QColor& wallpaper)
{
    const bool themeAsks
        = vela::themes::themed(theme, QStringLiteral("color-source"), light).toString() == QLatin1String("wallpaper");
    const bool fromWallpaper = source == Source::Wallpaper || (source == Source::Theme && themeAsks);
    if (fromWallpaper && wallpaper.isValid()) {
        return { wallpaper, Origin::Wallpaper };
    }
    return { chosen, Origin::Chosen };
}

// The whole rule, step 1 included: the accent the interface shows.
inline Resolved effectiveAccent(const QVariantMap& theme, bool light, Source source, const QColor& chosen,
    const QColor& wallpaper)
{
    const QVariant themed = vela::themes::themed(theme, QStringLiteral("accent"), light);
    if (themed.typeId() == QMetaType::QColor && themed.value<QColor>().isValid()) {
        return { themed.value<QColor>(), Origin::Theme };
    }
    return userAccent(theme, light, source, chosen, wallpaper);
}

// The source color of the tonal palette in a mode: the theme's `primary`,
// else the effective accent.
inline QColor schemeSource(const QVariantMap& theme, bool light, Source source, const QColor& chosen,
    const QColor& wallpaper)
{
    const QVariant primary = vela::themes::themed(theme, QStringLiteral("primary"), light);
    if (primary.typeId() == QMetaType::QColor && primary.value<QColor>().isValid()) {
        return primary.value<QColor>();
    }
    return effectiveAccent(theme, light, source, chosen, wallpaper).color;
}

} // namespace vela::accent
