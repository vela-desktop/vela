// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The wallpaper's source color for the tonal palette (accentsource.h):
// vela::color::sourceFromPixels over the picture read small. The shell
// computes it at each change of wallpaper (ShellController::wallpaperColor)
// and leaves it in mica.h's cache.

#include "quantize.h"

#include <QColor>
#include <QImage>
#include <QImageReader>
#include <QString>

#include <vector>

// Invalid when the picture can't be read or has no color for a palette (a
// grayscale or transparent one).
inline QColor wallpaperSourceColor(const QString& wallpaper)
{
    // Scaled down while reading, to at most 128 px on its longest side
    // (Vela's choice: enough for the quantizer to see every region of
    // color, fast enough to run at each change of wallpaper).
    QImageReader reader(wallpaper);
    QSize size = reader.size();
    if (!size.isValid()) {
        size = QSize(128, 128);
    }
    if (size.width() > 128 || size.height() > 128) {
        size.scale(128, 128, Qt::KeepAspectRatio);
    }
    reader.setScaledSize(size.expandedTo(QSize(1, 1)));
    const QImage image = reader.read().convertToFormat(QImage::Format_ARGB32);
    std::vector<vela::color::Argb> pixels;
    pixels.reserve(std::size_t(image.width()) * std::size_t(image.height()));
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        pixels.insert(pixels.end(), line, line + image.width());
    }
    // 0 (transparent) is never a result: it says the image has no color
    // for a palette.
    const vela::color::Argb source = vela::color::sourceFromPixels(pixels, 0);
    return source == 0 ? QColor() : QColor::fromRgb(source);
}
