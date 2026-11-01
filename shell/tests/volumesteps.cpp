// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests of the volume's steps (volumesteps.h): even numbers 2 points apart,
// the device's own positions when it has only a few.
//
//   ctest --test-dir build -R volumesteps      (or build/shell/vela-volumesteps-test)

#include "volumesteps.h"

#include <QTest>

namespace {

int percent(double volume)
{
    return int(std::lround(volume * 100.0));
}

} // namespace

class VolumeStepsTest : public QObject {
    Q_OBJECT

private slots:
    void intervalsFromPipeWire()
    {
        QCOMPARE(VolumeSteps::intervals(1.0 / 65536), 65535); // sound card
        QCOMPARE(VolumeSteps::intervals(1.0 / 128), 127); // Bluetooth A2DP
        QCOMPARE(VolumeSteps::intervals(1.0 / 16), 15); // Bluetooth HFP
        QCOMPARE(VolumeSteps::intervals(0.0), VolumeSteps::evenGrid); // unknown
    }

    void evenNumbersTwoPointsApart()
    {
        double volume = 0.0;
        for (int expected = 2; expected <= 100; expected += 2) {
            volume = VolumeSteps::step(volume, 1, 65535);
            QCOMPARE(percent(volume), expected);
        }
        QCOMPARE(VolumeSteps::step(volume, 1, 65535), 1.0); // the top stays there
        for (int expected = 98; expected >= 0; expected -= 2) {
            volume = VolumeSteps::step(volume, -1, 65535);
            QCOMPARE(percent(volume), expected);
        }
        QCOMPARE(VolumeSteps::step(volume, -1, 65535), 0.0);
        // Bluetooth A2DP: its positions are closer than 2 points.
        QCOMPARE(percent(VolumeSteps::step(0.50, 1, 127)), 52);
    }

    void oddNumberGoesBackOnTheGrid()
    {
        QCOMPARE(percent(VolumeSteps::step(0.33, 1, 65535)), 34);
        QCOMPARE(percent(VolumeSteps::step(0.33, -1, 65535)), 32);
        // Floating point: 0.58 * 50 is 28.999...
        QCOMPARE(percent(VolumeSteps::step(0.58, 1, 65535)), 60);
        QCOMPARE(percent(VolumeSteps::step(0.58, -1, 65535)), 56);
        // A hardware mixer in 1 dB steps (a FiiO K11): 96 is read back as
        // 96.0007, 94 could be 93.9993. Each still moves on.
        QCOMPARE(percent(VolumeSteps::step(0.960007, -1, 65535)), 94);
        QCOMPARE(percent(VolumeSteps::step(0.960007, 1, 65535)), 98);
        QCOMPARE(percent(VolumeSteps::step(0.939993, 1, 65535)), 96);
        QCOMPARE(percent(VolumeSteps::step(0.939993, -1, 65535)), 92);
        QCOMPARE(percent(VolumeSteps::snap(0.331, 65535)), 34);
        QCOMPARE(percent(VolumeSteps::snap(0.329, 65535)), 32);
    }

    void hardwareReadbackCloseToGridStillMoves()
    {
        // A requested 60% may read back as 59.6% (or 60.4%) after a
        // hardware mixer quantizes it. Both directions must leave the 60%
        // position, rather than re-requesting it.
        QCOMPARE(percent(VolumeSteps::step(0.596, 1, 65535)), 62);
        QCOMPARE(percent(VolumeSteps::step(0.596, -1, 65535)), 58);
        QCOMPARE(percent(VolumeSteps::step(0.604, 1, 65535)), 62);
        QCOMPARE(percent(VolumeSteps::step(0.604, -1, 65535)), 58);
        // Values genuinely between grid positions still move to the next
        // position in the chosen direction.
        QCOMPARE(percent(VolumeSteps::step(0.59, 1, 65535)), 60);
        QCOMPARE(percent(VolumeSteps::step(0.59, -1, 65535)), 58);
    }

    void quantizedHardwareReadbackNeverStalls()
    {
        // A device with 128 hardware positions, but a 2% UI grid.
        // Recalculate from the hardware feedback after every notch,
        // including notches that cross a direction change.
        for (int i = 0; i < 50; ++i) {
            const double reported = std::round((double(i) / 50.0) * 127.0) / 127.0;
            QCOMPARE(percent(VolumeSteps::step(reported, 1, 127)), (i + 1) * 2);
        }
        for (int i = 50; i > 0; --i) {
            const double reported = std::round((double(i) / 50.0) * 127.0) / 127.0;
            QCOMPARE(percent(VolumeSteps::step(reported, -1, 127)), (i - 1) * 2);
        }
    }

    void finerDevicesMoveOneNotchAtATime()
    {
        // The device sets the position nearest to the one asked for, up to
        // half of its own step away: 60% on 64 intervals reads back as
        // 59.375%. Every notch must still reach the next grid position.
        for (const int device : { 51, 64, 75, 83, 100, 127, 255, 65535 }) {
            const auto readback = [device](double volume) { return std::round(volume * device) / device; };
            double volume = 0.0;
            for (int notch = 1; notch <= 50; ++notch) {
                volume = readback(VolumeSteps::step(volume, 1, device));
                QVERIFY2(std::lround(volume * 50) == notch,
                    qPrintable(QStringLiteral("%1 intervals, up to notch %2: %3").arg(device).arg(notch).arg(volume)));
            }
            for (int notch = 49; notch >= 0; --notch) {
                volume = readback(VolumeSteps::step(volume, -1, device));
                QVERIFY2(std::lround(volume * 50) == notch,
                    qPrintable(QStringLiteral("%1 intervals, down to notch %2: %3").arg(device).arg(notch).arg(volume)));
            }
        }
    }

    void fewPositionsOneAtATime()
    {
        // A headset in call mode: 15 intervals, 6.67 points each.
        QCOMPARE(percent(VolumeSteps::step(0.0, 1, 15)), 7);
        QCOMPARE(percent(VolumeSteps::step(7.0 / 15, 1, 15)), 53);
        QCOMPARE(percent(VolumeSteps::step(7.0 / 15, -1, 15)), 40);
        // From a value between two positions, to the nearest one that way.
        QCOMPARE(percent(VolumeSteps::step(0.50, 1, 15)), 53);
        QCOMPARE(percent(VolumeSteps::step(0.50, -1, 15)), 47);
        QCOMPARE(percent(VolumeSteps::snap(0.45, 15)), 47);
    }
};

QTEST_GUILESS_MAIN(VolumeStepsTest)
#include "volumesteps.moc"
