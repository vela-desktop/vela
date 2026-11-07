// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The batteries UPower knows (docs/battery-notifications.md §3): its
// DisplayDevice (the PC's own batteries, together) and the peripherals it
// enumerates, HID++ and Bluetooth alike. Followed by signals, never polled:
// DeviceAdded and DeviceRemoved, each device's PropertiesChanged, UPower's
// owner (a restart starts over). Each device is read with GetAll, async;
// a reply older than the last request, or from a previous UPower, is
// dropped.

#include "batteryalerts.h"

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusObjectPath>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

class QDBusServiceWatcher;

class UPowerBatteries : public QObject, protected QDBusContext {
    Q_OBJECT

public:
    explicit UPowerBatteries(const QDBusConnection& bus = QDBusConnection::systemBus(), QObject* parent = nullptr);
    ~UPowerBatteries() override;

    // Starts following UPower; the readings come as signals.
    void start();

    // A device's properties (org.freedesktop.UPower.Device) as a reading.
    static BatteryReading read(const QString& path, const QVariantMap& properties);
    // Never a battery to tell about: line power, a UPS, a power supply
    // battery other than the DisplayDevice. It isn't followed any further.
    static bool ignored(const QString& path, const QVariantMap& properties);
    // The identity of a device: its vendor and serial, its native path, or
    // its object path (§4.3).
    static QString identity(const QString& path, const QVariantMap& properties);

    static const QString displayDevice;

Q_SIGNALS:
    void changed(const BatteryReading& reading);
    void removed(const QString& id);

private Q_SLOTS:
    void deviceAdded(const QDBusObjectPath& path);
    void deviceRemoved(const QDBusObjectPath& path);
    void propertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);

private:
    struct Device {
        quint64 request = 0; // the last read asked, unique across devices
        QString id; // fixed at the first reading, for as long as it stays
        bool followed = false; // its PropertiesChanged connected
        int failures = 0; // reads failed in a row
    };

    void enumerate();
    void addDevice(const QString& path);
    void removeDevice(const QString& path);
    void unfollow(const QString& path, Device& device);
    void refresh(const QString& path);
    void resetDevices();

    QDBusConnection m_bus;
    QDBusServiceWatcher* m_serviceWatcher = nullptr;
    QHash<QString, Device> m_devices;
    quint64 m_generation = 0;
    quint64 m_requests = 0;
};
