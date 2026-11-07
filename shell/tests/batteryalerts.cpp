// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The battery notifications' policy (docs/battery-notifications.md §4): how
// UPower's properties are read (UPowerBatteries::read), and when
// BatteryAlerts tells, stays quiet and rearms. No D-Bus here; the whole
// chain against a fake UPower is in batterynotices.cpp.

#include "batteryalerts.h"
#include "upowerbatteries.h"

#include <QTest>

#include <limits>

using Action = BatteryAlerts::Action;
using Level = BatteryAlerts::Level;
using Warning = BatteryReading::Warning;

namespace {

const QString mousePath = QStringLiteral("/org/freedesktop/UPower/devices/battery_hidpp_battery_0");

// A Logitech MX Vertical on a Unifying receiver, as UPower 1.91 shows it at
// low charge: the coarse level is the truth, its percentage a guess.
QVariantMap mx()
{
    return {
        { QStringLiteral("NativePath"), QStringLiteral("hidpp_battery_0") },
        { QStringLiteral("Vendor"), QStringLiteral("Logitech, Inc.") },
        { QStringLiteral("Model"), QStringLiteral("Logitech MX Vertical") },
        { QStringLiteral("Serial"), QStringLiteral("e6-43-fc-59") },
        { QStringLiteral("Type"), 5u },
        { QStringLiteral("PowerSupply"), false },
        { QStringLiteral("IsPresent"), true },
        { QStringLiteral("State"), 2u },
        { QStringLiteral("BatteryLevel"), 3u },
        { QStringLiteral("WarningLevel"), 3u },
        { QStringLiteral("Percentage"), 10.0 },
    };
}

QVariantMap laptop(double percentage, uint warning = 1, uint state = 2)
{
    return {
        { QStringLiteral("Type"), 2u },
        { QStringLiteral("PowerSupply"), true },
        { QStringLiteral("IsPresent"), true },
        { QStringLiteral("State"), state },
        { QStringLiteral("WarningLevel"), warning },
        { QStringLiteral("Percentage"), percentage },
    };
}

BatteryReading device(Warning warning, const QString& id = QStringLiteral("mouse"))
{
    BatteryReading reading;
    reading.id = id;
    reading.type = 5;
    reading.discharging = true;
    reading.warning = warning;
    return reading;
}

BatteryReading pc(double percentage, Warning warning = Warning::None)
{
    BatteryReading reading;
    reading.id = QStringLiteral("system");
    reading.system = true;
    reading.type = 2;
    reading.discharging = true;
    reading.percentage = percentage;
    reading.warning = warning;
    return reading;
}

BatteryReading charging(BatteryReading reading)
{
    reading.charging = true;
    reading.discharging = false;
    return reading;
}

// The actions as "show:<id>:low", "show:<id>:critical", "close:<id>".
QStringList said(const std::vector<Action>& actions)
{
    QStringList out;
    for (const Action& action : actions) {
        out << (action.kind == Action::Kind::Close
                ? QStringLiteral("close:") + action.reading.id
                : QStringLiteral("show:%1:%2").arg(action.reading.id,
                      action.level == Level::Critical ? QStringLiteral("critical") : QStringLiteral("low")));
    }
    return out;
}

} // namespace

class BatteryAlertsTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // --- UPower's properties ---

    void warningLevelIsTheSource()
    {
        BatteryReading reading = UPowerBatteries::read(mousePath, mx());
        QCOMPARE(reading.warning, Warning::Low);
        QVERIFY(reading.present && reading.discharging && !reading.charging && !reading.system);
        QCOMPARE(reading.name, QStringLiteral("Logitech MX Vertical"));

        // UPower decides, whatever the percentage says.
        QVariantMap p = mx();
        p[QStringLiteral("WarningLevel")] = 1u;
        p[QStringLiteral("Percentage")] = 1.0;
        QCOMPARE(UPowerBatteries::read(mousePath, p).warning, Warning::None);
        p[QStringLiteral("WarningLevel")] = 4u;
        p[QStringLiteral("Percentage")] = 90.0;
        QCOMPARE(UPowerBatteries::read(mousePath, p).warning, Warning::Critical);
        p[QStringLiteral("WarningLevel")] = 5u; // Action
        QCOMPARE(UPowerBatteries::read(mousePath, p).warning, Warning::Critical);
        p[QStringLiteral("WarningLevel")] = 2u; // Discharging, UPS only
        QCOMPARE(UPowerBatteries::read(mousePath, p).warning, Warning::None);
        p[QStringLiteral("WarningLevel")] = 0u;
        QCOMPARE(UPowerBatteries::read(mousePath, p).warning, Warning::Unknown);
        p.remove(QStringLiteral("WarningLevel"));
        QCOMPARE(UPowerBatteries::read(mousePath, p).warning, Warning::Unknown);
    }

    void unknownCoarseLevelIsNeverTrusted()
    {
        QVariantMap p = mx();
        p[QStringLiteral("BatteryLevel")] = 0u; // Unknown, not None
        p[QStringLiteral("WarningLevel")] = 4u;
        p[QStringLiteral("Percentage")] = 0.0;
        QCOMPARE(UPowerBatteries::read(mousePath, p).warning, Warning::Unknown);
    }

    void zeroBeforeTheFirstReading()
    {
        QVariantMap p = mx();
        p[QStringLiteral("BatteryLevel")] = 1u; // None: percentage-based
        p[QStringLiteral("WarningLevel")] = 4u;
        p[QStringLiteral("Percentage")] = 0.0;
        p[QStringLiteral("State")] = 0u;
        QCOMPARE(UPowerBatteries::read(mousePath, p).warning, Warning::Unknown);
        p[QStringLiteral("State")] = 2u; // really discharging: really empty
        QCOMPARE(UPowerBatteries::read(mousePath, p).warning, Warning::Critical);
    }

    void badPercentages()
    {
        for (const double bad : { -1.0, 120.0, std::numeric_limits<double>::quiet_NaN() }) {
            QVariantMap p = laptop(bad);
            QCOMPARE(UPowerBatteries::read(UPowerBatteries::displayDevice, p).percentage, -1.0);
        }
        QCOMPARE(UPowerBatteries::read(UPowerBatteries::displayDevice, laptop(42.0)).percentage, 42.0);
    }

    void presence()
    {
        // Bluetooth headsets leave IsPresent false while reporting a charge.
        QVariantMap headset {
            { QStringLiteral("Type"), 17u }, { QStringLiteral("IsPresent"), false },
            { QStringLiteral("WarningLevel"), 1u }, { QStringLiteral("Percentage"), 100.0 },
        };
        QVERIFY(UPowerBatteries::read(QStringLiteral("/h"), headset).present);
        // A battery bay that's empty is not.
        QVariantMap bay = mx();
        bay[QStringLiteral("Type")] = 2u;
        bay[QStringLiteral("IsPresent")] = false;
        QVERIFY(!UPowerBatteries::read(QStringLiteral("/b"), bay).present);
        bay[QStringLiteral("Type")] = 0u; // unknown kind
        QVERIFY(!UPowerBatteries::read(QStringLiteral("/b"), bay).present);

        // A desktop's DisplayDevice: unknown type, not present.
        QVariantMap desktop { { QStringLiteral("Type"), 0u }, { QStringLiteral("IsPresent"), false } };
        const BatteryReading none = UPowerBatteries::read(UPowerBatteries::displayDevice, desktop);
        QVERIFY(none.system && !none.present);
        QCOMPARE(none.id, QStringLiteral("system"));
        QVERIFY(UPowerBatteries::read(UPowerBatteries::displayDevice, laptop(50)).present);
    }

    void chargingStates()
    {
        for (const uint state : { 1u, 4u, 5u }) {
            const BatteryReading r = UPowerBatteries::read(UPowerBatteries::displayDevice, laptop(50, 1, state));
            QVERIFY(r.charging && !r.discharging);
        }
        for (const uint state : { 2u, 3u, 6u }) {
            const BatteryReading r = UPowerBatteries::read(UPowerBatteries::displayDevice, laptop(50, 1, state));
            QVERIFY(!r.charging && r.discharging);
        }
        const BatteryReading unknown = UPowerBatteries::read(UPowerBatteries::displayDevice, laptop(50, 1, 0));
        QVERIFY(!unknown.charging && !unknown.discharging);
    }

    void whatIsNeverFollowed()
    {
        QVERIFY(!UPowerBatteries::ignored(mousePath, mx()));
        // BAT0: in the DisplayDevice already.
        QVERIFY(UPowerBatteries::ignored(QStringLiteral("/org/freedesktop/UPower/devices/battery_BAT0"), laptop(50)));
        QVERIFY(!UPowerBatteries::ignored(UPowerBatteries::displayDevice, laptop(50)));
        QVERIFY(UPowerBatteries::ignored(QStringLiteral("/ac"), { { QStringLiteral("Type"), 1u } }));
        QVERIFY(UPowerBatteries::ignored(QStringLiteral("/ups"), { { QStringLiteral("Type"), 3u } }));
    }

    void identities()
    {
        QCOMPARE(UPowerBatteries::identity(mousePath, mx()), QStringLiteral("serial:Logitech, Inc.:e6-43-fc-59"));
        QVariantMap p = mx();
        p.remove(QStringLiteral("Serial"));
        QCOMPARE(UPowerBatteries::identity(mousePath, p), QStringLiteral("native:hidpp_battery_0"));
        p.remove(QStringLiteral("NativePath"));
        QCOMPARE(UPowerBatteries::identity(mousePath, p), QStringLiteral("path:") + mousePath);
    }

    // --- When to tell ---

    void lowOnceThenCritical()
    {
        BatteryAlerts alerts;
        QCOMPARE(said(alerts.update(device(Warning::None))), QStringList());
        QCOMPARE(said(alerts.update(device(Warning::Low))), QStringList { QStringLiteral("show:mouse:low") });
        QCOMPARE(said(alerts.update(device(Warning::Low))), QStringList());
        QCOMPARE(said(alerts.update(device(Warning::Critical))), QStringList { QStringLiteral("show:mouse:critical") });
        QCOMPARE(said(alerts.update(device(Warning::Critical))), QStringList());
        QCOMPARE(said(alerts.update(device(Warning::Low))), QStringList()); // no step back
    }

    void unknownChangesNothing()
    {
        BatteryAlerts alerts;
        alerts.update(device(Warning::Low));
        QCOMPARE(said(alerts.update(device(Warning::Unknown))), QStringList());
        QCOMPARE(said(alerts.update(device(Warning::Low))), QStringList());
        QCOMPARE(said(alerts.update(device(Warning::Critical))), QStringList { QStringLiteral("show:mouse:critical") });
    }

    // Voltage-based readings bounce: only one notification.
    void bouncingIsToldOnce()
    {
        BatteryAlerts alerts;
        QCOMPARE(said(alerts.update(device(Warning::Low))).size(), 1);
        for (int i = 0; i < 5; ++i) {
            QCOMPARE(said(alerts.update(device(Warning::None))), QStringList());
            QCOMPARE(said(alerts.update(device(Warning::Low))), QStringList());
        }
    }

    void chargingRearms()
    {
        BatteryAlerts alerts;
        alerts.update(device(Warning::Low));
        QCOMPARE(said(alerts.update(charging(device(Warning::Low)))), QStringList { QStringLiteral("close:mouse") });
        QCOMPARE(said(alerts.update(charging(device(Warning::None)))), QStringList());
        QCOMPARE(said(alerts.update(device(Warning::None))), QStringList());
        QCOMPARE(said(alerts.update(device(Warning::Low))), QStringList { QStringLiteral("show:mouse:low") });
    }

    void reconnecting()
    {
        BatteryAlerts alerts;
        alerts.update(device(Warning::Low));
        // Out of range and back as it was: not told again.
        alerts.removed(QStringLiteral("mouse"));
        QCOMPARE(said(alerts.update(device(Warning::Low))), QStringList());
        // Batteries replaced: back with a normal charge, rearmed.
        alerts.removed(QStringLiteral("mouse"));
        QCOMPARE(said(alerts.update(device(Warning::None))), QStringList { QStringLiteral("close:mouse") });
        QCOMPARE(said(alerts.update(device(Warning::Low))), QStringList { QStringLiteral("show:mouse:low") });
        // Back unknown at first: still told, and still back.
        alerts.removed(QStringLiteral("mouse"));
        QCOMPARE(said(alerts.update(device(Warning::Unknown))), QStringList());
        QCOMPARE(said(alerts.update(device(Warning::Low))), QStringList());
        alerts.removed(QStringLiteral("mouse"));
        QCOMPARE(said(alerts.update(device(Warning::Unknown))), QStringList());
        QCOMPARE(said(alerts.update(device(Warning::None))), QStringList { QStringLiteral("close:mouse") });
        // Rejoined is used up by the first known level.
        alerts.update(device(Warning::Low));
        alerts.update(device(Warning::None));
        QCOMPARE(said(alerts.update(device(Warning::Low))), QStringList());
    }

    void devicesAreApart()
    {
        BatteryAlerts alerts;
        QCOMPARE(said(alerts.update(device(Warning::Low, QStringLiteral("a")))).size(), 1);
        QCOMPARE(said(alerts.update(device(Warning::Low, QStringLiteral("b")))), QStringList { QStringLiteral("show:b:low") });
        QCOMPARE(said(alerts.update(charging(device(Warning::Low, QStringLiteral("a"))))), QStringList { QStringLiteral("close:a") });
        QCOMPARE(said(alerts.update(device(Warning::Low, QStringLiteral("b")))), QStringList());
    }

    // --- The PC's battery ---

    void systemFollowsTheThreshold()
    {
        BatteryAlerts alerts;
        QCOMPARE(said(alerts.update(pc(21))), QStringList());
        QCOMPARE(said(alerts.update(pc(20))), QStringList { QStringLiteral("show:system:low") });
        QCOMPARE(said(alerts.update(pc(12))), QStringList());
        // Critical is UPower's, whatever the percentage.
        QCOMPARE(said(alerts.update(pc(6, Warning::Critical))), QStringList { QStringLiteral("show:system:critical") });
        QCOMPARE(said(alerts.update(charging(pc(6)))), QStringList { QStringLiteral("close:system") });
        QCOMPARE(said(alerts.update(pc(30))), QStringList());
    }

    void systemIgnoresUPowersLow()
    {
        BatteryAlerts alerts;
        alerts.configure({ true, 10, true });
        QCOMPARE(said(alerts.update(pc(15, Warning::Low))), QStringList()); // the user's 10% rules
        QCOMPARE(said(alerts.update(pc(10, Warning::Low))), QStringList { QStringLiteral("show:system:low") });
    }

    void systemNeedsAReading()
    {
        BatteryAlerts alerts;
        BatteryReading unknownState = pc(5);
        unknownState.discharging = false; // e.g. plugged in at a charge limit
        QCOMPARE(said(alerts.update(unknownState)), QStringList());
        // Neither charging nor discharging: not even UPower's critical.
        unknownState.warning = Warning::Critical;
        QCOMPARE(said(alerts.update(unknownState)), QStringList());
        QCOMPARE(said(alerts.update(pc(-1))), QStringList());
        BatteryReading gone = pc(5);
        gone.present = false;
        QCOMPARE(said(alerts.update(gone)), QStringList());
    }

    void systemSwitchedOff()
    {
        BatteryAlerts alerts;
        alerts.update(pc(15));
        alerts.update(device(Warning::Low));
        // Everything about the PC's battery goes, critical too; devices stay.
        QCOMPARE(said(alerts.configure({ false, 20, true })), QStringList { QStringLiteral("close:system") });
        QCOMPARE(said(alerts.update(pc(4, Warning::Critical))), QStringList());
        // Back on: what the battery is at now is told.
        QCOMPARE(said(alerts.configure({ true, 20, true })), QStringList { QStringLiteral("show:system:critical") });
    }

    void devicesSwitchedOff()
    {
        BatteryAlerts alerts;
        alerts.update(pc(15));
        alerts.update(device(Warning::Low));
        QCOMPARE(said(alerts.configure({ true, 20, false })), QStringList { QStringLiteral("close:mouse") });
        QCOMPARE(said(alerts.update(device(Warning::Critical))), QStringList());
        QCOMPARE(said(alerts.configure({ true, 20, true })), QStringList { QStringLiteral("show:mouse:critical") });
    }

    void thresholdMoves()
    {
        BatteryAlerts alerts;
        QCOMPARE(said(alerts.update(pc(25))), QStringList());
        // Raised over the charge: told now.
        QCOMPARE(said(alerts.configure({ true, 30, true })), QStringList { QStringLiteral("show:system:low") });
        // Lowered under it: the warning goes, and comes at the new level.
        QCOMPARE(said(alerts.configure({ true, 10, true })), QStringList { QStringLiteral("close:system") });
        QCOMPARE(said(alerts.update(pc(15))), QStringList());
        QCOMPARE(said(alerts.update(pc(10))), QStringList { QStringLiteral("show:system:low") });
        // A critical one stays whatever the threshold.
        alerts.update(pc(4, Warning::Critical));
        QCOMPARE(said(alerts.configure({ true, 50, true })), QStringList());
        QCOMPARE(said(alerts.configure({ true, 5, true })), QStringList());
    }

    // The level lowered while the charge isn't known: the first known
    // reading decides, as if the level had moved then.
    void thresholdMovesWhileUnknown()
    {
        BatteryAlerts alerts;
        QCOMPARE(said(alerts.update(pc(18))), QStringList { QStringLiteral("show:system:low") });
        QCOMPARE(said(alerts.update(pc(-1))), QStringList());
        QCOMPARE(said(alerts.configure({ true, 10, true })), QStringList());
        QCOMPARE(said(alerts.update(pc(-1))), QStringList());
        QCOMPARE(said(alerts.update(pc(15))), QStringList { QStringLiteral("close:system") });
        QCOMPARE(said(alerts.update(pc(10))), QStringList { QStringLiteral("show:system:low") });
        // Still low at the new level when known: the warning stays.
        QCOMPARE(said(alerts.update(pc(-1))), QStringList());
        QCOMPARE(said(alerts.configure({ true, 5, true })), QStringList());
        QCOMPARE(said(alerts.configure({ true, 15, true })), QStringList());
        QCOMPARE(said(alerts.update(pc(9))), QStringList());
        QCOMPARE(said(alerts.update(pc(12))), QStringList());
    }

    void awayDevicesAreNotToldOnSettings()
    {
        BatteryAlerts alerts;
        alerts.configure({ true, 20, false });
        alerts.update(device(Warning::Low));
        alerts.removed(QStringLiteral("mouse"));
        QCOMPARE(said(alerts.configure({ true, 20, true })), QStringList());
    }
};

QTEST_GUILESS_MAIN(BatteryAlertsTest)
#include "batteryalerts.moc"
