// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Quick preview (Space): type classification and text truncation.
//   ctest --test-dir build -R files-peek

#include "peek.h"

#include <QTest>

class PeekTest : public QObject {
    Q_OBJECT
private slots:
    void classify()
    {
        QCOMPARE(peek::classify("inode/directory", true, false, true, false), QString("folder"));
        QCOMPARE(peek::classify("image/png", false, true, true, false), QString("image"));
        QCOMPARE(peek::classify("application/pdf", false, false, true, false), QString("pdf"));
        QCOMPARE(peek::classify("application/pdf", false, true, true, false), QString("pdf"));
        QCOMPARE(peek::classify("application/pdf", false, false, false, false), QString("other"));
        QCOMPARE(peek::classify("text/x-python", false, false, false, false), QString("text"));
        QCOMPARE(peek::classify("text/markdown", false, false, false, false), QString("text"));
        QCOMPARE(peek::classify("application/json", false, false, false, false), QString("text"));
        QCOMPARE(peek::classify("application/zip", false, false, false, false), QString("other"));
        QCOMPARE(peek::classify("video/mp4", false, false, false, true), QString("media"));
        QCOMPARE(peek::classify("audio/flac", false, false, false, true), QString("media"));
        QCOMPARE(peek::classify("video/mp4", false, false, false, false), QString("other"));
    }
    void sniff()
    {
        QVERIFY(peek::looksLikeText("[section]\nkey = value\n"));
        QVERIFY(peek::looksLikeText(QString("çalışma: ok").toUtf8()));
        QVERIFY(peek::looksLikeText(QString("aé").toUtf8().chopped(1))); // cut inside the last character
        QVERIFY(!peek::looksLikeText(QByteArray("\x89PNG\r\n\x1a\n\0\0", 10)));
        QVERIFY(!peek::looksLikeText(QByteArray("\xff\xfe\xfd text")));
        QVERIFY(!peek::looksLikeText(QByteArray()));
    }
    void truncateShort()
    {
        bool cut = true;
        QCOMPARE(peek::truncateText("a\tb", 3, 100, &cut), QString("a    b"));
        QVERIFY(!cut);
    }
    void truncateLimit()
    {
        bool cut = false;
        const QString t = peek::truncateText(QByteArray(200, 'x'), 1000, 100, &cut);
        QCOMPARE(t.size(), 100);
        QVERIFY(cut);
    }
    void truncateUtf8Boundary()
    {
        // "é" is 2 bytes: a limit of 3 would cut the second one in half.
        const QByteArray data = QString("aéé").toUtf8(); // 5 bytes
        const QString t = peek::truncateText(data, data.size(), 4, nullptr);
        QCOMPARE(t, QString("aé"));
    }
};

QTEST_MAIN(PeekTest)
#include "peek.moc"
