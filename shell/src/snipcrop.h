// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QtMath>

namespace vela {

// Crop a logical selection from a captured output, in upright pixel coordinates.
inline QImage cropSnipImage(const QImage& full, const QSize& logicalSize, const QRectF& selection)
{
    if (full.isNull() || logicalSize.isEmpty() || selection.isEmpty()) return {};

    const qreal sx = qreal(full.width()) / logicalSize.width();
    const qreal sy = qreal(full.height()) / logicalSize.height();
    // Round endpoints independently to avoid one-pixel errors at fractional scale.
    const int left = qRound(selection.left() * sx);
    const int top = qRound(selection.top() * sy);
    const int right = qRound((selection.x() + selection.width()) * sx);
    const int bottom = qRound((selection.y() + selection.height()) * sy);
    const QRect area = QRect(left, top, right - left, bottom - top).intersected(full.rect());

    // QImage::copy(QRect()) copies the *whole image*: never pass a null crop.
    if (area.isEmpty()) return {};
    return full.copy(area);
}

} // namespace vela
