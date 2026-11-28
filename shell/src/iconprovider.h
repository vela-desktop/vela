// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QQuickImageProvider>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

#include "kdeconfig.h"

// The icon theme chosen for Vela and KDE apps: [Icons] Theme in kdeglobals
// (Settings writes it, hicolor too when that is the choice), else the
// platform's, else VELA_ICON_THEME or Breeze Dark. Read again at every call:
// a choice made in Settings applies without a restart.
inline QString configuredIconTheme()
{
    const std::optional<QString> configured = vela::kde::read(
        vela::kde::configPath(QStringLiteral("kdeglobals")), QStringLiteral("Icons"), QStringLiteral("Theme"));
    if (configured && !configured->isEmpty()) {
        return *configured;
    }
    const QString platform = QIcon::themeName();
    if (platform.isEmpty() || platform == QLatin1String("hicolor")) {
        return qEnvironmentVariable("VELA_ICON_THEME", QStringLiteral("breeze-dark"));
    }
    return platform;
}

// The icon theme matching the light or dark mode: the "-dark" variant
// (breeze-dark) on dark, the normal one (breeze) on light, starting from the
// configured theme (which stays as it is: only Vela's apps take the variant).
// Call at startup, when the mode changes and when the theme does. With KDE's
// platform the icons come from KIconLoader, which colors the monochrome ones
// with the app's palette: that follows the mode too.
inline void applyIconTheme(bool light)
{
    // Qt's generic platform theme can advertise only :/icons (for example
    // on a standalone Wayland desktop without the KDE platform plugin).
    // Preserve platform/custom paths and also discover installed XDG themes.
    // Also ~/.icons, the legacy place Settings offers themes from.
    QStringList paths = QIcon::themeSearchPaths();
    QStringList wanted { QDir::homePath() + QStringLiteral("/.icons") };
    for (const QString& data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        wanted.append(data + QStringLiteral("/icons"));
    }
    for (const QString& icons : std::as_const(wanted)) {
        if (!paths.contains(icons)) {
            paths.append(icons);
        }
    }
    if (paths != QIcon::themeSearchPaths()) {
        QIcon::setThemeSearchPaths(paths);
    }
    QPalette palette = QGuiApplication::palette();
    const QColor text = light ? QColor(0x1b, 0x1b, 0x1b) : QColor(0xf3, 0xf3, 0xf6);
    const QColor background = light ? QColor(0xf3, 0xf3, 0xf3) : QColor(0x20, 0x20, 0x20);
    for (const QPalette::ColorGroup group : { QPalette::Active, QPalette::Inactive, QPalette::Disabled }) {
        palette.setColor(group, QPalette::WindowText, text);
        palette.setColor(group, QPalette::Text, text);
        palette.setColor(group, QPalette::ButtonText, text);
        palette.setColor(group, QPalette::Window, background);
        palette.setColor(group, QPalette::Base, background);
        palette.setColor(group, QPalette::Button, background);
    }
    const bool changed = palette != QGuiApplication::palette();
    if (changed) {
        QGuiApplication::setPalette(palette);
    }

    const QString base = configuredIconTheme();
    auto exists = [](const QString& name) {
        const QStringList paths = QIcon::themeSearchPaths();
        return std::any_of(paths.begin(), paths.end(),
            [&](const QString& dir) { return QFileInfo::exists(dir + u'/' + name + QStringLiteral("/index.theme")); });
    };
    QString plain = base;
    if (plain.endsWith(QLatin1String("-dark"))) {
        plain.chop(5);
    }
    const QString dark = plain + QStringLiteral("-dark");
    QString chosen = base;
    if (light && plain != base && exists(plain)) {
        chosen = plain;
    } else if (!light && dark != base && exists(dark)) {
        chosen = dark;
    }
    // Set again on a new palette too: Qt redraws the theme's icons.
    const bool themeChanged = QIcon::themeName() != chosen;
    if (changed || themeChanged) {
        QIcon::setThemeName(chosen);
        // Already loaded icons have the old colors or theme: they're
        // reloaded. KIconLoader (KDE's platform) empties its cache when it
        // gets the signal KDE's icon module sends.
        QPixmapCache::clear();
        vela::kde::iconLoaderChanged();
    }
}

// Makes the system theme's icons available to QML:
//   Image { source: "image://icon/" + encodeURIComponent("org.kde.dolphin")
//           sourceSize: Qt.size(width, height) }
// Accepts both theme icon names and absolute paths. The sourceSize is the
// logical size it's shown at: Qt multiplies it by the output scale and here
// it's drawn at exactly those pixels.
//
// Two modes: "icon" uses the theme's drawing for that size (command icons,
// monochrome and crisp when small); "fileicon" always uses the colored
// drawing of at least `designSize` pixels, carefully scaled down (files,
// folders and places, colored even when small, like in Windows).
//
// A prefix before the name: "v<n>/" the icon theme's revision (a new theme
// changes the address), then "l/" and "d/" say for which mode (light or dark)
// the icon is asked, so changing mode changes the address and QML reloads
// it; "w/" wants it white (on the accent); "t<rrggbb>/" wants it in that
// color (on a tonal fill, whose content may be dark in the dark mode, where
// the theme's drawings are light).
class IconProvider : public QQuickImageProvider {
public:
    explicit IconProvider(int designSize = 0)
        : QQuickImageProvider(QQuickImageProvider::Pixmap)
        , m_designSize(designSize)
    {
    }

    QPixmap requestPixmap(const QString& id, QSize* size, const QSize& requestedSize) override
    {
        const int extent = requestedSize.isValid()
            ? std::max({ requestedSize.width(), requestedSize.height(), 8 })
            : 64;
        // "v<n>/" first: the icon theme's revision (Theme.iconRevision), only
        // there so that a new theme changes the address and QML asks again.
        QString key = id;
        const qsizetype revision = key.indexOf(u'/');
        if (revision > 1 && key[0] == u'v'
            && std::all_of(key.cbegin() + 1, key.cbegin() + revision, [](QChar ch) { return ch.isDigit(); })) {
            key = key.mid(revision + 1);
        }
        QString name = QUrl::fromPercentEncoding(key.toUtf8());
        QColor tint;
        if (key.size() > 2 && key[1] == u'/' && (key[0] == u'l' || key[0] == u'd' || key[0] == u'w')) {
            if (key[0] == u'w') {
                tint = Qt::white;
            }
            name = QUrl::fromPercentEncoding(key.mid(2).toUtf8());
        } else if (key.size() > 8 && key[0] == u't' && key[7] == u'/') {
            const QColor asked = QColor::fromString(u'#' + key.mid(1, 6));
            if (asked.isValid()) {
                tint = asked;
                name = QUrl::fromPercentEncoding(key.mid(8).toUtf8());
            }
        }

        QIcon icon = name.startsWith(u'/') ? QIcon(name) : QIcon::fromTheme(name);
        if (icon.isNull()) {
            icon = QIcon::fromTheme(QStringLiteral("application-x-executable"));
        }
        // Exactly at the requested pixels: Qt has already multiplied the
        // sourceSize (the icon's logical size) by the output scale. Without
        // ratio 1, QIcon would multiply again by the output's integer scale
        // and the icon, shrunk on screen, would get blurry (especially at
        // fractional scales such as 125%).
        QPixmap pixmap;
        if (extent < m_designSize) {
            pixmap = icon.pixmap(QSize(m_designSize, m_designSize), 1.0)
                         .scaled(extent, extent, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        } else {
            pixmap = icon.pixmap(QSize(extent, extent), 1.0);
        }
        if (tint.isValid() && !pixmap.isNull()) {
            QPainter painter(&pixmap);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(pixmap.rect(), tint);
        }
        if (size) {
            *size = pixmap.size();
        }
        return pixmap;
    }

private:
    int m_designSize;
};
