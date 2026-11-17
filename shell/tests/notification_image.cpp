// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// "image-data" from any client on the session bus (src/notifications.cpp):
// a buffer that doesn't hold what the header says is refused, never read
// past its end.

#include "notifications.h"

#include <QTest>

#include <limits>

namespace {

QByteArray pixels(qsizetype size, char value = '\x7f')
{
    return QByteArray(size, value);
}

} // namespace

class NotificationImageTest : public QObject {
    Q_OBJECT

private slots:
    void tightRowsDecode()
    {
        const QImage image = decodeNotificationImage(3, 2, 3 * 4, 8, 4, pixels(3 * 4 * 2));
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(3, 2));
        QCOMPARE(image.format(), QImage::Format_RGBA8888);
    }

    void paddedRowsDecode()
    {
        // Rows of 9 bytes padded to 12; the last row needs no padding.
        const QImage image = decodeNotificationImage(3, 2, 12, 8, 3, pixels(12 + 9));
        QVERIFY(!image.isNull());
        QCOMPARE(image.format(), QImage::Format_RGB888);
    }

    void strideThatHidesRowsIsRefused_data()
    {
        QTest::addColumn<int>("rowStride");
        QTest::newRow("zero") << 0;
        QTest::newRow("negative") << -64;
        QTest::newRow("shorter than a row") << 8;
    }

    void strideThatHidesRowsIsRefused()
    {
        QFETCH(int, rowStride);
        // A few bytes: enough for the old size check with this stride, not for
        // the rows QImage would read.
        QVERIFY(decodeNotificationImage(64, 64, rowStride, 8, 4, pixels(64 * 4)).isNull());
    }

    void shortBufferIsRefused()
    {
        QVERIFY(decodeNotificationImage(4, 4, 16, 8, 4, pixels(16 * 4 - 1)).isNull());
    }

    void hugeOrEmptySidesAreRefused()
    {
        QVERIFY(decodeNotificationImage(maxNotificationImageSide + 1, 1, (maxNotificationImageSide + 1) * 4, 8, 4,
            pixels((maxNotificationImageSide + 1) * 4))
                    .isNull());
        QVERIFY(decodeNotificationImage(1, maxNotificationImageSide + 1, 4, 8, 4, pixels(4)).isNull());
        QVERIFY(decodeNotificationImage(0, 4, 0, 8, 4, pixels(64)).isNull());
        QVERIFY(decodeNotificationImage(4, -1, 16, 8, 4, pixels(64)).isNull());
    }

    void largestStrideDoesNotOverflow()
    {
        // stride × (height - 1) is computed in 64 bits: no wrap to a small size.
        QVERIFY(decodeNotificationImage(2, maxNotificationImageSide, std::numeric_limits<int>::max(), 8, 4,
            pixels(64))
                    .isNull());
    }

    void unsupportedLayoutsAreRefused()
    {
        QVERIFY(decodeNotificationImage(2, 2, 8, 16, 4, pixels(64)).isNull());
        QVERIFY(decodeNotificationImage(2, 2, 8, 8, 2, pixels(64)).isNull());
    }
};

QTEST_GUILESS_MAIN(NotificationImageTest)
#include "notification_image.moc"
