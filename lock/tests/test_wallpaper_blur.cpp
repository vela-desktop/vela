// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../wallpaperblur.h"

#include <QColor>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

int red(const QImage& image, int x, int y)
{
    return qRed(image.pixel(x, y));
}

void testDimensionsAndInputPreservation()
{
    QImage source(513, 287, QImage::Format_RGB32);
    for (int y = 0; y < source.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = qRgb(x % 256, y % 256, (x + y) % 256);
        }
    }
    const QImage unchanged = source.copy();
    const QImage result = vela::lock::blurWallpaper(source);
    check(result.size() == source.size(), "blur returns the full physical buffer dimensions");
    check(result.format() == QImage::Format_RGB32, "blur returns opaque RGB32");
    check(source == unchanged, "blur does not modify the source wallpaper");
    check(result == vela::lock::blurWallpaper(source), "blur and subtle dither are deterministic");
    check(qAlpha(result.pixel(256, 140)) == 255, "blurred wallpaper is opaque");
}

void testUniformAndSmallImages()
{
    for (const QSize size : { QSize(1, 1), QSize(2, 3), QSize(7, 9), QSize(320, 180) }) {
        QImage source(size, QImage::Format_RGB32);
        source.fill(QColor(35, 110, 210));
        const QImage result = vela::lock::blurWallpaper(source);
        check(result.size() == size, "small/uniform wallpaper dimensions are preserved");
        for (int y : { 0, size.height() / 2, size.height() - 1 }) {
            for (int x : { 0, size.width() / 2, size.width() - 1 }) {
                const QColor c(result.pixel(x, y));
                check(std::abs(c.red() - 35) <= 2 && std::abs(c.green() - 110) <= 2
                        && std::abs(c.blue() - 210) <= 2,
                    "uniform color stays uniform apart from at most 2 LSB of dither");
            }
        }
    }
    check(vela::lock::blurWallpaper(QImage()).isNull(), "null wallpaper produces a null image");
}

void testSmoothEdgeAndRetainedLargeShapes()
{
    QImage source(512, 256, QImage::Format_RGB32);
    for (int y = 0; y < source.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = x < source.width() / 2 ? qRgb(0, 0, 0) : qRgb(255, 255, 255);
        }
    }
    const QImage result = vela::lock::blurWallpaper(source);
    check(red(result, 24, 128) <= 2 && red(result, 488, 128) >= 253,
        "large-scale dark/light areas survive the blur");
    check(red(result, 256, 128) > 35 && red(result, 256, 128) < 220,
        "hard edge becomes a smooth transition");
    check(red(result, 230, 128) + 12 < red(result, 256, 128)
            && red(result, 256, 128) + 12 < red(result, 282, 128),
        "blur has a continuous, non-blocky transition across the edge");
    for (int x = 208; x < 304; x += 3) {
        check(red(result, x, 128) <= red(result, x + 3, 128) + 4,
            "blur transition has no large steps or ringing");
    }
}

void testHighFrequencyTextureReduction()
{
    QImage source(384, 216, QImage::Format_RGB32);
    for (int y = 0; y < source.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const int value = ((x / 2 + y / 2) % 2) ? 255 : 0;
            row[x] = qRgb(value, value, value);
        }
    }
    const QImage result = vela::lock::blurWallpaper(source);
    for (int y = 60; y < 150; y += 11) {
        for (int x = 80; x < 300; x += 13) {
            check(std::abs(red(result, x, y) - 128) <= 8,
                "fine texture is hidden by the password-screen blur");
        }
    }
}

} // namespace

int main()
{
    testDimensionsAndInputPreservation();
    testUniformAndSmallImages();
    testSmoothEdgeAndRetainedLargeShapes();
    testHighFrequencyTextureReduction();
    if (failures) {
        std::cerr << failures << " wallpaper blur assertions failed\n";
        return 1;
    }
    std::cout << "Wallpaper blur: all assertions passed\n";
    return 0;
}
