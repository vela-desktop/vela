// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vela::lock {
namespace detail {

// A single clamped-edge, constant-time-per-pixel box pass. Repeated passes
// approximate a Gaussian without forcing the image down to a tiny thumbnail.
// The image passed here is always RGB32 and already downsampled to 1/4 size.
inline QImage boxBlurPass(const QImage& source, int radius, bool horizontal)
{
    const int width = source.width();
    const int height = source.height();
    QImage result(source.size(), QImage::Format_RGB32);
    const int count = radius * 2 + 1;

    if (horizontal) {
        for (int y = 0; y < height; ++y) {
            const auto* input = reinterpret_cast<const QRgb*>(source.constScanLine(y));
            auto* output = reinterpret_cast<QRgb*>(result.scanLine(y));
            int sumR = 0;
            int sumG = 0;
            int sumB = 0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const QRgb pixel = input[std::clamp(offset, 0, width - 1)];
                sumR += qRed(pixel);
                sumG += qGreen(pixel);
                sumB += qBlue(pixel);
            }
            for (int x = 0; x < width; ++x) {
                output[x] = qRgb((sumR + count / 2) / count, (sumG + count / 2) / count,
                    (sumB + count / 2) / count);
                const QRgb leaving = input[std::clamp(x - radius, 0, width - 1)];
                const QRgb entering = input[std::clamp(x + radius + 1, 0, width - 1)];
                sumR += qRed(entering) - qRed(leaving);
                sumG += qGreen(entering) - qGreen(leaving);
                sumB += qBlue(entering) - qBlue(leaving);
            }
        }
    } else {
        for (int x = 0; x < width; ++x) {
            const auto sample = [&](int y) {
                const auto* row = reinterpret_cast<const QRgb*>(
                    source.constScanLine(std::clamp(y, 0, height - 1)));
                return row[x];
            };
            int sumR = 0;
            int sumG = 0;
            int sumB = 0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const QRgb pixel = sample(offset);
                sumR += qRed(pixel);
                sumG += qGreen(pixel);
                sumB += qBlue(pixel);
            }
            for (int y = 0; y < height; ++y) {
                auto* output = reinterpret_cast<QRgb*>(result.scanLine(y));
                output[x] = qRgb((sumR + count / 2) / count, (sumG + count / 2) / count,
                    (sumB + count / 2) / count);
                const QRgb leaving = sample(y - radius);
                const QRgb entering = sample(y + radius + 1);
                sumR += qRed(entering) - qRed(leaving);
                sumG += qGreen(entering) - qGreen(leaving);
                sumB += qBlue(entering) - qBlue(leaving);
            }
        }
    }
    return result;
}

} // namespace detail

// Produce the password-screen wallpaper; draw() caches it by physical size
// AND output scale across login keystrokes and authentication states.
// The old implementation rebuilt the image from a 1/32-sized thumbnail.
// Three pairs of box passes at 1/4 size instead produce sigma ~= 26 *scale*
// physical pixels, i.e. about 26 logical pixels on every output.
inline QImage blurWallpaper(const QImage& image, uint32_t scale120 = 120)
{
    if (image.isNull()) {
        return {};
    }

    QImage reduced = image;
    for (int pass = 0; pass < 2; ++pass) {
        if (reduced.width() == 1 && reduced.height() == 1) {
            break;
        }
        reduced = reduced.scaled(std::max(1, reduced.width() / 2),
            std::max(1, reduced.height() / 2), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    reduced = reduced.convertToFormat(QImage::Format_RGB32);

    // Qt/Wayland physical pixels increase with output scale, while the
    // login UI is specified in logical pixels. Keep the visual defocus
    // consistent (the integer radius approximates a 26-logical-pixel sigma).
    // Cap extreme/malformed scales to avoid pathological work and overflow.
    const int radius = std::clamp(int(std::lround(6.0 * double(scale120) / 120.0)), 1, 64);
    for (int pass = 0; pass < 3; ++pass) {
        reduced = detail::boxBlurPass(reduced, radius, true);
        reduced = detail::boxBlurPass(reduced, radius, false);
    }

    QImage result = reduced.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                        .convertToFormat(QImage::Format_RGB32);

    // Deterministic low-amplitude dither reduces visible banding in gradients
    // underneath the existing dark overlay, without introducing texture blocks.
    uint32_t seed = 0x9e3779b9u;
    for (int y = 0; y < result.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            const int noise = int(seed % 5u) - 2;
            const QRgb pixel = line[x];
            line[x] = qRgb(std::clamp(qRed(pixel) + noise, 0, 255),
                std::clamp(qGreen(pixel) + noise, 0, 255),
                std::clamp(qBlue(pixel) + noise, 0, 255));
        }
    }
    return result;
}

} // namespace vela::lock
