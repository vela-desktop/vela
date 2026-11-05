// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "peripheralbatterymonitor.h"

#include "notifications.h"

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

QVariant value(const QVariantMap& properties, const char* name)
{
    const QVariant property = properties.value(QString::fromLatin1(name));
    if (property.metaType().id() == qMetaTypeId<QDBusVariant>()) {
        return property.value<QDBusVariant>().variant();
    }
    return property;
}

QString stableId(const QString& path, const QVariantMap& properties)
{
    const QString serial = value(properties, "Serial").toString();
    if (!serial.isEmpty()) {
        // A UPower object path can change on reconnect (mouse_... vs battery_...).
        return QStringLiteral("serial:%1:%2:%3")
            .arg(value(properties, "Vendor").toString(), value(properties, "Model").toString(), serial);
    }
    const QString native = value(properties, "NativePath").toString();
    return native.isEmpty() ? QStringLiteral("path:") + path : QStringLiteral("native:") + native;
}

} // namespace

PeripheralBatteryMonitor::PeripheralBatteryMonitor(NotificationServer* notifications,
    const QDBusConnection& bus, QObject* parent)
    : QObject(parent)
    , m_notifications(notifications)
    , m_bus(bus)
{
    m_serviceWatcher = new QDBusServiceWatcher(service, m_bus,
        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
        [this](const QString&, const QString&, const QString& newOwner) {
            resetDevices();
            if (!newOwner.isEmpty()) {
                enumerate();
            }
        });

    m_bus.connect(service, root, service, QStringLiteral("DeviceAdded"),
        this, SLOT(deviceAdded(QDBusObjectPath)));
    m_bus.connect(service, root, service, QStringLiteral("DeviceRemoved"),
        this, SLOT(deviceRemoved(QDBusObjectPath)));

    // Dismissal doesn't re-arm an alert; only recovery/charging can do that.
    connect(m_notifications, &NotificationServer::NotificationClosed, this,
        [this](uint id, uint) {
            for (auto it = m_alerts.begin(); it != m_alerts.end(); ++it) {
                if (it->notification == id) {
                    it->notification = 0;
                }
            }
        });
    enumerate();
}

PeripheralBatteryMonitor::~PeripheralBatteryMonitor()
{
    resetDevices();
}

PeripheralBatteryMonitor::Level PeripheralBatteryMonitor::classify(const QVariantMap& properties)
{
    if (value(properties, "PowerSupply").toBool()
        || (value(properties, "IsPresent").isValid() && !value(properties, "IsPresent").toBool())) {
        return Level::Unknown;
    }

    const uint type = value(properties, "Type").toUInt();
    if (type == 0 || type == 1 || type == 3) { // unknown, line power, UPS
        return Level::Unknown;
    }

    const uint state = value(properties, "State").toUInt();
    if (state == 1 || state == 4 || state == 5) { // charging, fully charged, pending charge
        return Level::Normal; // a new charge cycle rearms the notifications
    }

    // UPower's BatteryLevel: Unknown=0, None=1, Low=3, Critical=4,
    // Normal=6, High=7, Full=8. A non-None coarse level is authoritative.
    const QVariant batteryLevel = value(properties, "BatteryLevel");
    if (batteryLevel.isValid()) {
        switch (batteryLevel.toUInt()) {
        case 0: return Level::Unknown;
        case 3: return Level::Low;
        case 4: return Level::Critical;
        case 6:
        case 7:
        case 8: return Level::Normal;
        case 1: break; // no coarse-level reporting: percentage is usable
        default: return Level::Unknown;
        }
    }

    // WarningLevel can be meaningful for devices with no coarse battery level.
    const uint warning = value(properties, "WarningLevel").toUInt();
    if (warning == 4 || warning == 5) {
        return Level::Critical;
    }
    if (warning == 3) {
        return Level::Low;
    }

    const QVariant percentage = value(properties, "Percentage");
    bool valid = false;
    const double amount = percentage.toDouble(&valid);
    if (!valid || !std::isfinite(amount) || amount < 0.0 || amount > 100.0
        || (amount == 0.0 && state != 2 && state != 3)) {
        return Level::Unknown;
    }
    if (amount <= 10.0) {
        return Level::Critical;
    }
    if (amount <= 20.0) {
        return Level::Low;
    }
    return Level::Normal;
}

void PeripheralBatteryMonitor::enumerate()
{
    const quint64 generation = m_generation;
    const QDBusMessage call = QDBusMessage::createMethodCall(
        service, root, service, QStringLiteral("EnumerateDevices"));
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, generation] {
            const QDBusPendingReply<QList<QDBusObjectPath>> reply = *watcher;
            watcher->deleteLater();
            if (generation != m_generation || reply.isError()) {
                return; // UPower may not be running, or has restarted
            }
            for (const QDBusObjectPath& path : reply.value()) {
                addDevice(path.path());
            }
        });
}

void PeripheralBatteryMonitor::deviceAdded(const QDBusObjectPath& path)
{
    addDevice(path.path());
}

void PeripheralBatteryMonitor::deviceRemoved(const QDBusObjectPath& path)
{
    removeDevice(path.path());
}

void PeripheralBatteryMonitor::addDevice(const QString& path)
{
    if (path.isEmpty() || path.endsWith(QLatin1String("/DisplayDevice")) || m_devices.contains(path)) {
        return;
    }
    m_devices.insert(path, {});
    m_bus.connect(service, path, propertiesInterface, QStringLiteral("PropertiesChanged"),
        this, SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
    refresh(path);
}

void PeripheralBatteryMonitor::removeDevice(const QString& path)
{
    if (m_devices.remove(path) == 0) {
        return;
    }
    m_bus.disconnect(service, path, propertiesInterface, QStringLiteral("PropertiesChanged"),
        this, SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
    // Keep m_alerts: reconnecting or restarting UPower must not warn again.
}

void PeripheralBatteryMonitor::resetDevices()
{
    ++m_generation; // discard replies from the previous UPower owner
    const QStringList paths = m_devices.keys();
    for (const QString& path : paths) {
        removeDevice(path);
    }
}

void PeripheralBatteryMonitor::propertiesChanged(const QString& interface,
    const QVariantMap&, const QStringList&)
{
    if (interface != deviceInterface) {
        return;
    }
    // Qt's generic D-Bus signal slot has no device-path argument. Refresh the
    // small tracked set; each query is async and older replies are discarded.
    const QStringList paths = m_devices.keys();
    for (const QString& path : paths) {
        refresh(path);
    }
}

void PeripheralBatteryMonitor::refresh(const QString& path)
{
    auto it = m_devices.find(path);
    if (it == m_devices.end()) {
        return;
    }
    const quint64 request = ++it->request;
    const quint64 generation = m_generation;
    QDBusMessage call = QDBusMessage::createMethodCall(
        service, path, propertiesInterface, QStringLiteral("GetAll"));
    call << deviceInterface;
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, path, request, generation] {
            const QDBusPendingReply<QVariantMap> reply = *watcher;
            watcher->deleteLater();
            const auto it = m_devices.constFind(path);
            if (generation != m_generation || it == m_devices.cend()
                || it->request != request || reply.isError()) {
                return;
            }
            observe(path, reply.value());
        });
}

void PeripheralBatteryMonitor::observe(const QString& path, const QVariantMap& properties)
{
    const Level level = classify(properties);
    if (level == Level::Unknown) {
        return; // never show 0%, nor reset previously issued warnings
    }
    const QString key = stableId(path, properties);
    auto existing = m_alerts.find(key);
    if (level == Level::Normal) {
        if (existing != m_alerts.end()) {
            if (existing->notification != 0) {
                m_notifications->CloseNotification(existing->notification);
            }
            m_alerts.erase(existing);
        }
        return;
    }

    Alert& alert = m_alerts[key];
    if (int(level) <= int(alert.highestTold)) {
        return;
    }

    // A critical alert must get its own popup even when the earlier low
    // notification has already moved to history (or DND routed it there).
    if (alert.notification != 0) {
        m_notifications->CloseNotification(alert.notification);
    }

    QString name = value(properties, "Model").toString();
    if (name.isEmpty()) {
        name = tr("Wireless device");
    }
    const bool critical = level == Level::Critical;
    const QString title = critical ? tr("%1 battery is critical").arg(name)
                                   : tr("%1 battery is low").arg(name);
    const QString body = critical ? tr("Recharge the device now.")
                                  : tr("Recharge the device soon.");
    const QVariantMap hints { { QStringLiteral("urgency"), critical ? 2 : 1 } };
    alert.notification = m_notifications->Notify(tr("Devices"), 0,
        critical ? QStringLiteral("battery-empty") : QStringLiteral("battery-caution"),
        title, body, {}, hints, critical ? 0 : 8000);
    alert.highestTold = level;
}
