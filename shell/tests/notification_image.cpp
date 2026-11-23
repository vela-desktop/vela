// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// "image-data" from any client on the session bus (src/notifications.cpp):
// a buffer that doesn't hold what the header says is refused, never read
// past its end.

#include "notifications.h"

#include <QColor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingReply>
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
        // Rows of 9 bytes padded to 12; the last row has no padding, as
        // libnotify sends it. Nothing past the buffer is read (ASan), the
        // padding stays out and each row lands where it belongs.
        QByteArray data = pixels(12, '\x01');
        data.replace(9, 3, QByteArray(3, '\xee'));
        data += pixels(9, '\x02');
        const QImage image = decodeNotificationImage(3, 2, 12, 8, 3, data);
        QVERIFY(!image.isNull());
        QCOMPARE(image.format(), QImage::Format_RGB888);
        QCOMPARE(image.pixelColor(2, 0), QColor(1, 1, 1));
        QCOMPARE(image.pixelColor(0, 1), QColor(2, 2, 2));
        QCOMPARE(image.pixelColor(2, 1), QColor(2, 2, 2));
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

    void largeIconsKeepAspectRatioAtMaximumDisplayScale()
    {
        const QImage image = decodeNotificationImage(1024, 512, 4096, 8, 4, pixels(4096 * 512));
        QCOMPARE(image.size(), QSize(448, 224));
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

    void replacementsReleaseThePreviousImage()
    {
        QDBusConnection serverBus = QDBusConnection::sessionBus();
        if (!serverBus.isConnected()) QSKIP("requires a session bus");
        const QString path = QStringLiteral("/ImageTest");
        NotificationServer server;
        QVERIFY(serverBus.registerObject(path, &server, QDBusConnection::ExportScriptableSlots));
        const QDBusConnection client =
            QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("image-test"));
        QDBusArgument hint;
        hint.beginStructure();
        hint << 64 << 32 << 256 << true << 8 << 4 << pixels(256 * 32);
        hint.endStructure();
        const QVariantMap withPixels { { QStringLiteral("image-data"), QVariant::fromValue(hint) } };
        uint id = 0;
        for (bool history : { false, true }) {
            QDBusMessage notify = QDBusMessage::createMethodCall(serverBus.baseService(), path,
                QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
            notify << QStringLiteral("test") << id << QString() << QStringLiteral("summary") << QString()
                   << QStringList() << withPixels << 0;
            QDBusPendingReply<uint> reply = client.asyncCall(notify);
            QTRY_VERIFY(reply.isFinished());
            QVERIFY2(!reply.isError(), qPrintable(reply.error().message()));
            if (id) QCOMPARE(reply.value(), id);
            id = reply.value();
            QCOMPARE(server.image(id).size(), QSize(64, 32));
            if (history) server.collectPopups();
            QCOMPARE(server.Notify("test", id, "dialog-information", "replacement", {}, {}, {}, 0), id);
            QVERIFY(server.image(id).isNull());
        }
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
