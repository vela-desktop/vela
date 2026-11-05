// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Low/critical battery notifications for mice, keyboards, headsets and other
// peripherals. UPower is the sole source; there is no vendor-specific backend.

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class NotificationServer;
class QDBusServiceWatcher;

class PeripheralBatteryMonitor : public QObject {
    Q_OBJECT

public:
    enum class Level { Unknown = -1, Normal = 0, Low = 1, Critical = 2 };

    explicit PeripheralBatteryMonitor(NotificationServer* notifications,
        const QDBusConnection& bus = QDBusConnection::systemBus(), QObject* parent = nullptr);
    ~PeripheralBatteryMonitor() override;

    // The UPower policy is kept separate from D-Bus for deterministic tests.
    // BatteryLevel=Unknown MUST NOT fall back to Percentage; BatteryLevel=None may.
    static Level classify(const QVariantMap& properties);

private Q_SLOTS:
    void deviceAdded(const QDBusObjectPath& path);
    void deviceRemoved(const QDBusObjectPath& path);
    void propertiesChanged(const QString& interface, const QVariantMap& changed,
        const QStringList& invalidated);

private:
    struct Device {
        quint64 request = 0;
    };
    struct Alert {
        Level highestTold = Level::Normal;
        uint notification = 0;
    };

    void enumerate();
    void addDevice(const QString& path);
    void removeDevice(const QString& path);
    void refresh(const QString& path);
    void observe(const QString& path, const QVariantMap& properties);
    void resetDevices();

    NotificationServer* m_notifications;
    QDBusConnection m_bus;
    QDBusServiceWatcher* m_serviceWatcher = nullptr;
    QHash<QString, Device> m_devices;
    // Retained across UPower restarts and path changes; cleared on recovery.
    QHash<QString, Alert> m_alerts;
    quint64 m_generation = 0;
};
