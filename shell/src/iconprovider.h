// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QQuickImageProvider>
#include <QUrl>

#include <algorithm>

// The icon theme matching the light or dark mode: the "-dark" variant
// (breeze-dark) on dark, the normal one (breeze) on light, starting from the
// theme chosen in KDE. Call at startup and when the mode changes. With KDE's
// platform the icons come from KIconLoader, which colors the monochrome ones
// with the app's palette: that follows the mode too.
inline void applyIconTheme(bool light)
{
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

    static const QString base = [] {
        QString theme = QIcon::themeName();
        if (theme.isEmpty() || theme == QLatin1String("hicolor")) {
            theme = qEnvironmentVariable("VELA_ICON_THEME", QStringLiteral("breeze-dark"));
        }
        return theme;
    }();
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
    if (QIcon::themeName() != chosen) {
        QIcon::setThemeName(chosen);
    } else if (changed) {
        QIcon::setThemeName(chosen);
    }
    if (changed) {
        // Already loaded icons have the old colors: they're reloaded.
        // KIconLoader (KDE's platform) empties its cache when it gets the
        // signal KDE's icon module sends.
        QPixmapCache::clear();
        QDBusConnection::sessionBus().send(QDBusMessage::createSignal(QStringLiteral("/KIconLoader"),
            QStringLiteral("org.kde.KIconLoader"), QStringLiteral("iconChanged"))
            << 0);
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
// A prefix before the name: "l/" and "d/" say for which mode (light or dark)
// the icon is asked, so changing mode changes the address and QML reloads
// it; "w/" wants it white (on the accent); "k/" wants the theme's white
// monochrome glyphs drawn dark, colored icons as they are (a surface that is
// always light: Windows 7's menus, a style's).
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
        QString name = QUrl::fromPercentEncoding(id.toUtf8());
        bool white = false;
        bool dark = false;
        if (id.size() > 2 && id[1] == u'/' && (id[0] == u'l' || id[0] == u'd' || id[0] == u'w' || id[0] == u'k')) {
            white = id[0] == u'w';
            dark = id[0] == u'k';
            name = QUrl::fromPercentEncoding(id.mid(2).toUtf8());
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
        if (white && !pixmap.isNull()) {
            QPainter painter(&pixmap);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(pixmap.rect(), Qt::white);
        }
        if (dark && !pixmap.isNull() && isWhiteGlyph(pixmap.toImage())) {
            QPainter painter(&pixmap);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(pixmap.rect(), QColor(0x1B, 0x1B, 0x1B));
        }
        if (size) {
            *size = pixmap.size();
        }
        return pixmap;
    }

private:
    // A white (or nearly) monochrome glyph: nine in ten of its opaque pixels
    // are light gray. Colored icons are not.
    static bool isWhiteGlyph(const QImage& source)
    {
        const QImage image = source.convertToFormat(QImage::Format_ARGB32);
        int opaque = 0;
        int whitish = 0;
        for (int y = 0; y < image.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(line[x]) > 128) {
                    ++opaque;
                    const int hi = std::max({ qRed(line[x]), qGreen(line[x]), qBlue(line[x]) });
                    const int lo = std::min({ qRed(line[x]), qGreen(line[x]), qBlue(line[x]) });
                    whitish += lo > 200 && hi - lo < 30 ? 1 : 0;
                }
            }
        }
        return opaque > 0 && whitish * 10 >= opaque * 9;
    }

    int m_designSize;
};
