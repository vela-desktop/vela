// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// When a battery is told about (docs/battery-notifications.md §4): the PC's
// own battery and the peripherals' (mice, keyboards, headsets...), as
// UPower reports them. Pure state, no D-Bus and no notifications: readings
// go in, what to show or close comes out. UPowerBatteries makes the
// readings, BatteryNotices carries out the actions.

#include <QHash>
#include <QString>

#include <optional>
#include <vector>

// One battery as UPower reports it, already sorted out.
struct BatteryReading {
    // UPower's own WarningLevel, the one source for peripherals (§4.1):
    // Unknown when UPower doesn't know, or its coarse BatteryLevel says
    // Unknown.
    enum class Warning { Unknown, None, Low, Critical };

    QString id; // stable while the device stays (§4.3); "system" for the PC
    QString name; // UPower's Model, may be empty
    uint type = 0; // UPower's Type: 5 mouse, 6 keyboard, 17 headset...
    bool system = false; // the PC's own batteries (UPower's DisplayDevice)
    bool present = true;
    bool charging = false; // charging, fully charged, waiting to charge
    bool discharging = false; // discharging, empty, waiting to discharge
    Warning warning = Warning::Unknown;
    double percentage = -1; // 0-100, -1 if not a valid reading
};

class BatteryAlerts {
public:
    enum class Level { Normal, Low, Critical };

    // What the user chose (vela-shell.conf, [battery]).
    struct Settings {
        bool system = true; // battery/notify: the PC's battery, both levels
        int systemLow = 20; // battery/lowLevel, percent
        bool devices = true; // battery/devices: the peripherals'
        bool operator==(const Settings&) const = default;
    };

    struct Action {
        enum class Kind { Show, Close };
        Kind kind;
        Level level; // Show: Low or Critical
        BatteryReading reading; // Close: only its id counts
    };

    // A new reading of a device (the same id: the same device).
    std::vector<Action> update(const BatteryReading& reading);
    // The device went away (unplugged, out of range, UPower restarted). What
    // was told stays told: it comes back as it left, nothing is said again;
    // it comes back with a normal charge (batteries replaced), it's rearmed.
    std::vector<Action> removed(const QString& id);
    // New settings: what they turn off is closed, what they turn on (or a
    // threshold that now covers the charge) is told.
    std::vector<Action> configure(const Settings& settings);

    const Settings& settings() const { return m_settings; }

    // The level a reading is at with these settings; nullopt: nothing to go
    // by (unknown, not present, its notifications turned off).
    static std::optional<Level> classify(const BatteryReading& reading, const Settings& settings);

private:
    struct Device {
        BatteryReading last;
        Level told = Level::Normal; // Normal: armed
        bool away = false;
        bool rejoined = false; // back after away, until a level is known
    };
    std::vector<Action> evaluate(Device& device);
    static std::vector<Action> rearm(Device& device);

    Settings m_settings;
    QHash<QString, Device> m_devices;
};
