// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "upowerbatteries.h"

#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>

#include <cmath>

namespace {

const QString service = QStringLiteral("org.freedesktop.UPower");
const QString root = QStringLiteral("/org/freedesktop/UPower");
const QString deviceInterface = QStringLiteral("org.freedesktop.UPower.Device");
const QString propertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

// UPower's Type, State, BatteryLevel and WarningLevel values.
enum Type : uint { Unknown = 0, LinePower = 1, Battery = 2, Ups = 3 };
enum State : uint { Charging = 1, Discharging = 2, Empty = 3, FullyCharged = 4, PendingCharge = 5, PendingDischarge = 6 };
enum Level : uint { LevelUnknown = 0, LevelNone = 1, WarningDischarging = 2, LevelLow = 3, LevelCritical = 4, LevelAction = 5 };

QVariant value(const QVariantMap& properties, const char* name)
{
    const QVariant property = properties.value(QString::fromLatin1(name));
    if (property.metaType().id() == qMetaTypeId<QDBusVariant>()) {
        return property.value<QDBusVariant>().variant();
    }
    return property;
}

} // namespace

const QString UPowerBatteries::displayDevice = QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice");

UPowerBatteries::UPowerBatteries(const QDBusConnection& bus, QObject* parent)
    : QObject(parent)
    , m_bus(bus)
{
}

UPowerBatteries::~UPowerBatteries()
{
    resetDevices();
}

void UPowerBatteries::start()
{
    if (m_serviceWatcher) {
        return;
    }
    m_serviceWatcher = new QDBusServiceWatcher(service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
        [this](const QString&, const QString&, const QString& newOwner) {
            resetDevices();
            if (!newOwner.isEmpty()) {
                enumerate();
            }
        });
    m_bus.connect(service, root, service, QStringLiteral("DeviceAdded"), this, SLOT(deviceAdded(QDBusObjectPath)));
    m_bus.connect(service, root, service, QStringLiteral("DeviceRemoved"), this, SLOT(deviceRemoved(QDBusObjectPath)));
    enumerate();
}

QString UPowerBatteries::identity(const QString& path, const QVariantMap& properties)
{
    const QString serial = value(properties, "Serial").toString();
    if (!serial.isEmpty()) {
        // A device's object path can change when it reconnects
        // (hidpp_battery_0, then _1); its serial doesn't.
        return QStringLiteral("serial:%1:%2").arg(value(properties, "Vendor").toString(), serial);
    }
    const QString native = value(properties, "NativePath").toString();
    return native.isEmpty() ? QStringLiteral("path:") + path : QStringLiteral("native:") + native;
}

bool UPowerBatteries::ignored(const QString& path, const QVariantMap& properties)
{
    if (path == displayDevice) {
        return false;
    }
    const uint type = value(properties, "Type").toUInt();
    // The PC's own batteries (BAT0, BAT1) come together in the DisplayDevice.
    return type == LinePower || type == Ups || value(properties, "PowerSupply").toBool();
}

BatteryReading UPowerBatteries::read(const QString& path, const QVariantMap& properties)
{
    BatteryReading reading;
    reading.system = path == displayDevice;
    reading.id = reading.system ? QStringLiteral("system") : identity(path, properties);
    reading.name = value(properties, "Model").toString().trimmed();
    reading.type = value(properties, "Type").toUInt();

    const QVariant isPresent = value(properties, "IsPresent");
    if (reading.system) {
        // A desktop's DisplayDevice is there too, of unknown type, not present.
        reading.present = reading.type == Battery && isPresent.toBool();
    } else {
        // IsPresent only means something for a battery bay: Bluetooth
        // headsets leave it false while reporting their charge.
        reading.present = reading.type != Unknown && !(reading.type == Battery && isPresent.isValid() && !isPresent.toBool());
    }

    const uint state = value(properties, "State").toUInt();
    reading.charging = state == Charging || state == FullyCharged || state == PendingCharge;
    reading.discharging = state == Discharging || state == Empty || state == PendingDischarge;

    bool valid = false;
    const double percentage = value(properties, "Percentage").toDouble(&valid);
    if (valid && std::isfinite(percentage) && percentage >= 0.0 && percentage <= 100.0) {
        reading.percentage = percentage;
    }

    // UPower's WarningLevel already weighs the coarse BatteryLevel and its
    // own thresholds (UPower.conf). A coarse level of Unknown, though, is
    // never trusted: its percentage is a guess ("should be ignored").
    const QVariant batteryLevel = value(properties, "BatteryLevel");
    const QVariant warning = value(properties, "WarningLevel");
    if (batteryLevel.isValid() && batteryLevel.toUInt() == LevelUnknown) {
        reading.warning = BatteryReading::Warning::Unknown;
    } else if (warning.isValid()) {
        switch (warning.toUInt()) {
        case LevelNone:
        case WarningDischarging: reading.warning = BatteryReading::Warning::None; break;
        case LevelLow: reading.warning = BatteryReading::Warning::Low; break;
        case LevelCritical:
        case LevelAction: reading.warning = BatteryReading::Warning::Critical; break;
        default: reading.warning = BatteryReading::Warning::Unknown; break;
        }
    }
    // A peripheral just connected can say 0% before its first real reading.
    const bool coarse = batteryLevel.isValid() && batteryLevel.toUInt() != LevelNone;
    if (!reading.system && !coarse && reading.percentage == 0.0 && !reading.discharging) {
        reading.warning = BatteryReading::Warning::Unknown;
    }
    return reading;
}

void UPowerBatteries::enumerate()
{
    const quint64 generation = m_generation;
    const QDBusMessage call = QDBusMessage::createMethodCall(service, root, service, QStringLiteral("EnumerateDevices"));
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation] {
        const QDBusPendingReply<QList<QDBusObjectPath>> reply = *watcher;
        watcher->deleteLater();
        if (generation != m_generation || reply.isError()) {
            return; // UPower isn't there, or restarted meanwhile
        }
        addDevice(displayDevice); // not among the enumerated ones
        for (const QDBusObjectPath& path : reply.value()) {
            addDevice(path.path());
        }
    });
}

void UPowerBatteries::deviceAdded(const QDBusObjectPath& path)
{
    addDevice(path.path());
}

void UPowerBatteries::deviceRemoved(const QDBusObjectPath& path)
{
    removeDevice(path.path());
}

void UPowerBatteries::addDevice(const QString& path)
{
    if (path.isEmpty() || m_devices.contains(path)) {
        return;
    }
    Device& device = m_devices[path];
    device.followed = m_bus.connect(service, path, propertiesInterface, QStringLiteral("PropertiesChanged"), this,
        SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
    refresh(path);
}

void UPowerBatteries::unfollow(const QString& path, Device& device)
{
    if (device.followed) {
        m_bus.disconnect(service, path, propertiesInterface, QStringLiteral("PropertiesChanged"), this,
            SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
        device.followed = false;
    }
}

void UPowerBatteries::removeDevice(const QString& path)
{
    auto it = m_devices.find(path);
    if (it == m_devices.end()) {
        return;
    }
    unfollow(path, *it);
    const QString id = it->id;
    m_devices.erase(it);
    if (!id.isEmpty()) {
        Q_EMIT removed(id);
    }
}

void UPowerBatteries::resetDevices()
{
    ++m_generation; // replies from the previous UPower are dropped
    const QStringList paths = m_devices.keys();
    for (const QString& path : paths) {
        removeDevice(path);
    }
}

void UPowerBatteries::propertiesChanged(const QString& interface, const QVariantMap&, const QStringList&)
{
    // Only the device that sent it: the signal's own path.
    if (interface == deviceInterface && calledFromDBus()) {
        refresh(message().path());
    }
}

void UPowerBatteries::refresh(const QString& path)
{
    auto it = m_devices.find(path);
    if (it == m_devices.end()) {
        return;
    }
    const quint64 request = ++it->request;
    const quint64 generation = m_generation;
    QDBusMessage call = QDBusMessage::createMethodCall(service, path, propertiesInterface, QStringLiteral("GetAll"));
    call << deviceInterface;
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path, request, generation] {
        const QDBusPendingReply<QVariantMap> reply = *watcher;
        watcher->deleteLater();
        auto it = m_devices.find(path);
        if (generation != m_generation || it == m_devices.end() || it->request != request || reply.isError()) {
            return;
        }
        const QVariantMap properties = reply.value();
        if (ignored(path, properties)) {
            unfollow(path, *it); // kept, so DeviceAdded doesn't add it again
            return;
        }
        BatteryReading reading = read(path, properties);
        if (it->id.isEmpty()) {
            it->id = reading.id;
        }
        reading.id = it->id; // a serial seen later doesn't change who it is
        Q_EMIT changed(reading);
    });
}
