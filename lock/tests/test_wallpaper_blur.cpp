// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../wallpaperblur.h"

#include <QColor>
#include <QImage>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string_view>

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
        const int delta = red(result, x + 3, 128) - red(result, x, 128);
        check(delta >= -4, "blur transition has no significant backward ringing");
        check(delta <= 20, "blur transition has no large positive pixel steps");
    }
}

QImage sharpEdge(int width, int height)
{
    QImage source(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < width; ++x) {
            row[x] = x < width / 2 ? qRgb(0, 0, 0) : qRgb(255, 255, 255);
        }
    }
    return source;
}

int firstAbove(const QImage& image, int y, int threshold)
{
    for (int x = 0; x < image.width(); ++x) {
        if (red(image, x, y) >= threshold) {
            return x;
        }
    }
    return image.width();
}

void testLogicalBlurWidthAcrossScales()
{
    double baselineWidth = 0.0;
    for (const uint32_t scale120 : { 120u, 150u, 180u, 210u, 240u }) {
        // All images depict the same 512x256 logical surface.
        const int width = 512 * int(scale120) / 120;
        const int height = 256 * int(scale120) / 120;
        const QImage result = vela::lock::blurWallpaper(sharpEdge(width, height), scale120);
        const int y = height / 2;
        const int first32 = firstAbove(result, y, 32);
        const int first224 = firstAbove(result, y, 224);
        check(first32 > 0 && first224 < width && first224 > first32,
            "both edge transition thresholds are visible");
        const double logicalWidth = double(first224 - first32) * 120.0 / scale120;
        if (scale120 == 120) {
            baselineWidth = logicalWidth;
        } else {
            check(std::abs(logicalWidth - baselineWidth) <= 6.0,
                "password blur width stays consistent in logical pixels across scales");
        }
    }
}

void testSmoothGradient()
{
    QImage source(512, 256, QImage::Format_RGB32);
    for (int y = 0; y < source.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const int value = 255 * x / (source.width() - 1);
            row[x] = qRgb(value, value, value);
        }
    }
    const QImage result = vela::lock::blurWallpaper(source);
    for (int x = 40; x < source.width() - 43; x += 3) {
        const int delta = red(result, x + 3, 128) - red(result, x, 128);
        check(delta >= -4 && delta <= 12, "gradients stay smooth without coarse steps");
    }
}

void benchmarkResolution(const QSize& size)
{
    QImage source(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            row[x] = qRgb(x % 256, y % 256, (x + y) % 256);
        }
    }
    const auto begin = std::chrono::steady_clock::now();
    const QImage blurred = vela::lock::blurWallpaper(source);
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin);
    check(blurred.size() == size, "benchmark preserves output dimensions");
    std::cout << size.width() << 'x' << size.height() << ": "
              << elapsed.count() << " ms (one-time blur generation)\n";
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

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
        for (const QSize size : { QSize(1920, 1080), QSize(2560, 1440), QSize(3840, 2160) }) {
            benchmarkResolution(size);
        }
        return failures ? 1 : 0;
    }
    if (argc != 1) {
        std::cerr << "Usage: vela-lock-blur-tests [--benchmark]\n";
        return 2;
    }
    testDimensionsAndInputPreservation();
    testUniformAndSmallImages();
    testSmoothEdgeAndRetainedLargeShapes();
    testLogicalBlurWidthAcrossScales();
    testSmoothGradient();
    testHighFrequencyTextureReduction();
    if (failures) {
        std::cerr << failures << " wallpaper blur assertions failed\n";
        return 1;
    }
    std::cout << "Wallpaper blur: all assertions passed\n";
    return 0;
}
