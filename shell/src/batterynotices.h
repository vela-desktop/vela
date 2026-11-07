// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The battery notifications (docs/battery-notifications.md §5): UPower's
// readings (UPowerBatteries) through BatteryAlerts, shown in Vela's
// notification center. Low ones respect Do not disturb; critical ones pass
// it. The PC's critical one stays until dismissed, a peripheral's expires
// into the notification center like any other.

#include "batteryalerts.h"

#include <QDBusConnection>
#include <QHash>
#include <QObject>

class NotificationServer;
class UPowerBatteries;

class BatteryNotices : public QObject {
    Q_OBJECT

public:
    explicit BatteryNotices(NotificationServer* notifications,
        const QDBusConnection& bus = QDBusConnection::systemBus(), QObject* parent = nullptr);
    ~BatteryNotices() override;

    // Starts following UPower; configure() first, so the first readings
    // already follow the user's settings.
    void start();
    void configure(const BatteryAlerts::Settings& settings);

    // The notification shown for a device's id, 0 if none (the tests').
    uint notification(const QString& id) const { return m_shown.value(id); }
    // Readings carried out so far (the tests wait for them).
    int readings() const { return m_readings; }

private:
    void carryOut(const std::vector<BatteryAlerts::Action>& actions);
    void show(const BatteryReading& reading, BatteryAlerts::Level level);

    NotificationServer* m_notifications;
    UPowerBatteries* m_batteries;
    BatteryAlerts m_alerts;
    QHash<QString, uint> m_shown; // device id -> notification
    int m_readings = 0;
};
