// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Deterministic policy tests. In particular, an MX Vertical can report a
// coarse Normal/Unknown level alongside an indicative "50% (should be
// ignored)" percentage: the level always wins.

#include "peripheralbatterymonitor.h"

#include <QTest>
#include <limits>

using Level = PeripheralBatteryMonitor::Level;

class PeripheralBatteriesTest : public QObject {
    Q_OBJECT

    static QVariantMap mouse()
    {
        return {
            { QStringLiteral("Type"), 5u }, // Mouse
            { QStringLiteral("PowerSupply"), false },
            { QStringLiteral("IsPresent"), true },
            { QStringLiteral("State"), 2u }, // Discharging
            { QStringLiteral("BatteryLevel"), 6u }, // Normal
            { QStringLiteral("Percentage"), 50.0 },
            { QStringLiteral("WarningLevel"), 1u },
        };
    }

private Q_SLOTS:
    void coarseLevelWinsOverPercentage()
    {
        QVariantMap p = mouse();
        p[QStringLiteral("Percentage")] = 1.0;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Normal);
        p[QStringLiteral("BatteryLevel")] = 3u; // Low
        p[QStringLiteral("Percentage")] = 90.0;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Low);
        p[QStringLiteral("BatteryLevel")] = 4u; // Critical
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Critical);
        p[QStringLiteral("BatteryLevel")] = 7u; // High
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Normal);
    }

    void unknownNeverFallsBackToPercentage()
    {
        QVariantMap p = mouse();
        p[QStringLiteral("BatteryLevel")] = 0u; // Unknown != None
        p[QStringLiteral("Percentage")] = 0.0;
        p[QStringLiteral("WarningLevel")] = 4u;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Unknown);
    }

    void percentageFallbackOnlyWhenCoarseReportingAbsent()
    {
        QVariantMap p = mouse();
        p[QStringLiteral("BatteryLevel")] = 1u; // None
        p[QStringLiteral("Percentage")] = 20.0;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Low);
        p[QStringLiteral("Percentage")] = 10.0;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Critical);
        p[QStringLiteral("Percentage")] = 21.0;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Normal);
        p.remove(QStringLiteral("BatteryLevel")); // old UPower
        p[QStringLiteral("Percentage")] = 5.0;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Critical);
    }

    void explicitWarningWhenNoCoarseBatteryLevel()
    {
        QVariantMap p = mouse();
        p[QStringLiteral("BatteryLevel")] = 1u;
        p[QStringLiteral("Percentage")] = 80.0;
        p[QStringLiteral("WarningLevel")] = 3u;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Low);
        p[QStringLiteral("WarningLevel")] = 5u;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Critical);
    }

    void badReadingsAreIgnored()
    {
        QVariantMap p = mouse();
        p[QStringLiteral("BatteryLevel")] = 1u;
        p[QStringLiteral("Percentage")] = -1.0;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Unknown);
        p[QStringLiteral("Percentage")] = 120.0;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Unknown);
        p[QStringLiteral("Percentage")] = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Unknown);
        p[QStringLiteral("Percentage")] = 0.0;
        p[QStringLiteral("State")] = 0u;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Unknown);
        p[QStringLiteral("State")] = 2u;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Critical);
    }

    void chargingOrMissingDoesNotWarn()
    {
        QVariantMap p = mouse();
        p[QStringLiteral("BatteryLevel")] = 4u;
        p[QStringLiteral("State")] = 1u;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Normal);
        p[QStringLiteral("State")] = 4u;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Normal);
        p[QStringLiteral("State")] = 2u;
        p[QStringLiteral("IsPresent")] = false;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Unknown);
    }

    void desktopBatteryAndUnknownDevicesAreIgnored()
    {
        QVariantMap p = mouse();
        p[QStringLiteral("PowerSupply")] = true;
        p[QStringLiteral("BatteryLevel")] = 4u;
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Unknown);
        p[QStringLiteral("PowerSupply")] = false;
        p[QStringLiteral("Type")] = 1u; // Line power
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Unknown);
        p[QStringLiteral("Type")] = 0u; // Unknown
        QVERIFY(PeripheralBatteryMonitor::classify(p) == Level::Unknown);
    }
};

QTEST_GUILESS_MAIN(PeripheralBatteriesTest)
#include "peripheralbatteries.moc"
