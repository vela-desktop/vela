// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests of the Snipping Tool's delay cycle and color format (snipextras.h).
//
//   ctest --test-dir build -R snipextras

#include "snipextras.h"

#include <QTest>

class SnipExtrasTest : public QObject {
    Q_OBJECT

private slots:
    void delayCycles()
    {
        int seconds = 0;
        const int expected[] = { 3, 5, 10, 0, 3 };
        for (int want : expected) {
            seconds = SnipExtras::nextDelay(seconds);
            QCOMPARE(seconds, want);
        }
        QCOMPARE(SnipExtras::nextDelay(7), 0); // unknown values fall back to None
    }

    void hexFormat()
    {
        QCOMPARE(SnipExtras::hexColor(QColor(255, 0, 128)), QStringLiteral("#FF0080"));
        QCOMPARE(SnipExtras::hexColor(QColor(0, 0, 0)), QStringLiteral("#000000"));
        QCOMPARE(SnipExtras::hexColor(QColor(1, 2, 3, 40)), QStringLiteral("#010203")); // alpha dropped
    }

    void recordGeometry()
    {
        QCOMPARE(SnipExtras::recordGeometry(QRect(10, 20, 640, 480)), QStringLiteral("10,20 640x480"));
    }

    void recordFileName()
    {
        const QDateTime when(QDate(2026, 10, 14), QTime(9, 5, 7));
        QCOMPARE(SnipExtras::recordFileName(when, false), QStringLiteral("Screen Recording 2026-10-14 09-05-07.mp4"));
        QCOMPARE(SnipExtras::recordFileName(when, true), QStringLiteral("Screen Recording 2026-10-14 09-05-07.webm"));
    }

    void recordArgs()
    {
        const QRect area(1, 2, 30, 40);
        QCOMPARE(SnipExtras::recordArgs("a.mp4", "", area, false, false),
            (QStringList { "-f", "a.mp4", "-g", "1,2 30x40", "-c", "libx264" }));
        QCOMPARE(SnipExtras::recordArgs("a.mp4", "DP-1", area, true, false),
            (QStringList { "-f", "a.mp4", "-o", "DP-1", "--audio", "-c", "libx264" }));
        QCOMPARE(SnipExtras::recordArgs("a.webm", "", area, true, true),
            (QStringList { "-f", "a.webm", "-g", "1,2 30x40", "--audio", "-c", "libvpx", "-C", "libopus" }));
        QVERIFY(SnipExtras::nextAudio(false));
        QVERIFY(!SnipExtras::nextAudio(true));
    }
};

QTEST_APPLESS_MAIN(SnipExtrasTest)
#include "snipextras.moc"
