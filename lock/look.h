// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the lock screen takes from the theme and the accent (lock/main.cpp
// draws with it): the effective accent, by the rule Style follows
// (shell/src/accentsource.h), and under controls-variant=expressive Vela
// Expressive's look, with the tonal roles of that accent (Tonal Spot,
// common/color/scheme.h). Pure functions over their inputs, for
// lock/tests/test_lock_look.cpp; configuredLook() reads the files.
//
// The lock must lock whatever it finds: a missing or broken theme, settings
// or wallpaper cache gives today's look (Vela's blue, the default
// drawing), never an error.

#include "accentsource.h"
#include "defaults.h"
#include "language.h"
#include "mica.h"
#include "scheme.h"
#include "themefile.h"

#include <QColor>
#include <QImage>
#include <QRect>
#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace vela::lock {

struct Look {
    // The accent the interface shows (the avatar, the field's line).
    QColor accent = vela::defaults::accent();
    // controls-variant=expressive: the expressive clock, field and avatar.
    bool expressive = false;
    // The dark scheme's roles (the lock draws light on a dimmed wallpaper
    // in both modes, Vela's choice), set when expressive.
    QColor primary;
    QColor primaryContainer;
    QColor onPrimaryContainer;
    QColor surfaceContainerHighest;
    QColor onSurface;
    QColor onSurfaceVariant;
};

inline QColor fromArgb(vela::color::Argb argb)
{
    return QColor::fromRgb(argb | 0xff000000u);
}

// The look of a theme (its merged tokens) for a mode, with the user's
// accent settings and the wallpaper's cached color (invalid: none).
inline Look lookFrom(const QVariantMap& theme, bool light, vela::accent::Source source, QColor chosen,
    const QColor& wallpaper)
{
    Look look;
    if (!chosen.isValid()) {
        chosen = vela::defaults::accent();
    }
    const QColor accent = vela::accent::effectiveAccent(theme, light, source, chosen, wallpaper).color;
    if (accent.isValid()) {
        look.accent = accent;
    }
    look.expressive = vela::themes::themed(theme, QStringLiteral("controls-variant"), light).toString()
        == QLatin1String("expressive");
    if (look.expressive) {
        QColor schemeSource = vela::accent::schemeSource(theme, light, source, chosen, wallpaper);
        if (!schemeSource.isValid()) {
            schemeSource = look.accent;
        }
        const vela::color::Roles roles = vela::color::tonalSpot(schemeSource.rgb(), true);
        look.primary = fromArgb(roles.primary);
        look.primaryContainer = fromArgb(roles.primaryContainer);
        look.onPrimaryContainer = fromArgb(roles.onPrimaryContainer);
        look.surfaceContainerHighest = fromArgb(roles.surfaceContainerHighest);
        look.onSurface = fromArgb(roles.onSurface);
        look.onSurfaceVariant = fromArgb(roles.onSurfaceVariant);
    }
    return look;
}

// The look the session has: vela.conf's theme, vela-shell.conf's accent
// and the shell's wallpaper color cache, as Style reads them
// (shell/src/appearance.cpp, the shell's mode).
inline Look configuredLook()
{
    QSettings shell(QStringLiteral("Vela"), QStringLiteral("vela-shell"));
    const bool light = shell.value(QStringLiteral("appearance/shellTheme")).toString() == QLatin1String("light");
    const QColor chosen(shell.value(QStringLiteral("appearance/accent")).toString());
    const vela::accent::Source source
        = vela::accent::sourceFromString(shell.value(QStringLiteral("appearance/accentSource")).toString());
    const QVariantMap theme = vela::themes::loadConfigured(vela::language::configPath()).merged();
    return lookFrom(theme, light, source, chosen, cachedWallpaperColor());
}

// WCAG's relative luminance and contrast ratio (1 to 21).
inline double luminance(const QColor& color)
{
    const auto channel = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
}

inline double contrast(const QColor& a, const QColor& b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// The lightest color of `behind` in `box` (physical pixels) as the clock
// sees it, under the clock's dimming (black at `dim` alpha): what text must
// read against. Sampled on a grid: the blurred wallpaper has no detail
// finer than it.
inline QColor brightestBehind(const QImage& behind, QRect box, int dim)
{
    box = box.intersected(behind.rect());
    if (box.isEmpty()) {
        return QColor(Qt::black);
    }
    const double keep = 1.0 - std::clamp(dim, 0, 255) / 255.0;
    QColor brightest(Qt::black);
    double most = -1;
    const int step = std::max(1, std::min(box.width(), box.height()) / 16);
    for (int y = box.top(); y <= box.bottom(); y += step) {
        for (int x = box.left(); x <= box.right(); x += step) {
            const QRgb pixel = behind.pixel(x, y);
            const QColor dimmed(int(qRed(pixel) * keep), int(qGreen(pixel) * keep), int(qBlue(pixel) * keep));
            const double l = luminance(dimmed);
            if (l > most) {
                most = l;
                brightest = dimmed;
            }
        }
    }
    return brightest;
}

// The expressive clock's color: the dark scheme's primary (tone 80), made
// lighter in its own hue and chroma (tones 85, 90, 95) until it reads at
// 3:1 (M3's minimum for large text, tokens.md) against `background`; white
// if none does.
inline QColor clockColor(const QColor& primary, const QColor& background)
{
    constexpr double minimum = 3.0;
    if (!primary.isValid()) {
        return QColor(Qt::white);
    }
    if (contrast(primary, background) >= minimum) {
        return primary;
    }
    const vela::color::Hct hct = vela::color::hctFromArgb(primary.rgb());
    for (const double tone : { 85.0, 90.0, 95.0 }) {
        const QColor lighter = fromArgb(vela::color::argbFromHct(hct.hue, hct.chroma, std::max(tone, hct.tone)));
        if (contrast(lighter, background) >= minimum) {
            return lighter;
        }
    }
    return QColor(Qt::white);
}

} // namespace vela::lock
