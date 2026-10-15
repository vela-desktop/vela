// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snipcrop.h"

#include <QTest>

class SnipCropTest : public QObject {
    Q_OBJECT

private slots:
    void collapsedSelectionDoesNotCopyTheScreen()
    {
        QImage full(4, 4, QImage::Format_RGB32);
        full.fill(Qt::red);
        // Two logical units round to zero pixels at a 0.1 pixel/logical ratio.
        QVERIFY(vela::cropSnipImage(full, QSize(40, 40), QRectF(11, 11, 2, 2)).isNull());
    }

    void completelyOutsideSelectionDoesNotCopyTheScreen()
    {
        QImage full(10, 8, QImage::Format_RGB32);
        full.fill(Qt::red);
        QVERIFY(vela::cropSnipImage(full, QSize(10, 8), QRectF(-20, -20, 2, 2)).isNull());
    }

    void fractionalScaleRoundsEndpoints()
    {
        QImage full(10, 8, QImage::Format_RGB32);
        for (int y = 0; y < full.height(); ++y)
            for (int x = 0; x < full.width(); ++x)
                full.setPixel(x, y, qRgb(x * 20, y * 30, 0));

        // Logical 8x6 maps to 10x8 physical pixels.
        const QImage crop = vela::cropSnipImage(full, QSize(8, 6), QRectF(1, 1, 3, 2));
        QVERIFY(!crop.isNull());
        QCOMPARE(crop, full.copy(QRect(1, 1, 4, 3)));
    }

    void partiallyOutsideSelectionClipsToSource()
    {
        QImage full(10, 8, QImage::Format_RGB32);
        full.fill(Qt::blue);
        const QImage crop = vela::cropSnipImage(full, QSize(10, 8), QRectF(-3, -2, 6, 5));
        QCOMPARE(crop, full.copy(QRect(0, 0, 3, 3)));
    }
};

QTEST_GUILESS_MAIN(SnipCropTest)
#include "snipcrop.moc"
