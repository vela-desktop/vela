// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bluetooth.h"

#include "bluetoothpower.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>

#include <algorithm>

using ManagedObjects = QMap<QDBusObjectPath, QMap<QString, QVariantMap>>;
using InterfaceMap = QMap<QString, QVariantMap>;
Q_DECLARE_METATYPE(ManagedObjects)
Q_DECLARE_METATYPE(InterfaceMap)

namespace {

const QString bluez = QStringLiteral("org.bluez");
const QString adapterInterface = QStringLiteral("org.bluez.Adapter1");
const QString deviceInterface = QStringLiteral("org.bluez.Device1");
const QString batteryInterface = QStringLiteral("org.bluez.Battery1");
const QString propertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

// BlueZ errors, in plain words when they're the common ones.
QString describe(const QDBusError& error)
{
    const QString name = error.name();
    if (name.endsWith(QLatin1String("AuthenticationFailed")) || name.endsWith(QLatin1String("AuthenticationRejected"))) {
        return QCoreApplication::translate("Bluetooth", "Pairing failed: the device rejected it.");
    }
    if (name.endsWith(QLatin1String("AuthenticationTimeout")) || name.endsWith(QLatin1String("ConnectionAttemptFailed"))
        || error.message().contains(QLatin1String("page-timeout"))) {
        return QCoreApplication::translate("Bluetooth", "The device isn't responding. Make sure it's on and nearby.");
    }
    if (name.endsWith(QLatin1String("AlreadyExists"))) {
        return {};
    }
    if (name.endsWith(QLatin1String("NotReady"))) {
        return QCoreApplication::translate("Bluetooth", "Bluetooth is turned off.");
    }
    return error.message().isEmpty() ? QCoreApplication::translate("Bluetooth", "The operation failed.") : error.message();
}

} // namespace

Bluetooth::Bluetooth(QObject* parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<ManagedObjects>();
    qDBusRegisterMetaType<InterfaceMap>();
}

void Bluetooth::refresh()
{
    QDBusConnection system = QDBusConnection::systemBus();
    QDBusInterface manager(bluez, QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), system);
    const QDBusReply<ManagedObjects> objects = manager.call(QStringLiteral("GetManagedObjects"));
    if (!objects.isValid()) {
        return;
    }
    m_devices.clear();
    m_adapter.clear();
    const ManagedObjects all = objects.value();
    for (auto it = all.cbegin(); it != all.cend(); ++it) {
        onInterfacesAdded(it.key(), it.value());
    }
    if (!m_watching) {
        m_watching = true;
        system.connect(bluez, QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
            QStringLiteral("InterfacesAdded"), this, SLOT(onInterfacesAdded(QDBusObjectPath, QMap<QString, QVariantMap>)));
        system.connect(bluez, QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
            QStringLiteral("InterfacesRemoved"), this, SLOT(onInterfacesRemoved(QDBusObjectPath, QStringList)));
        // All BlueZ objects: the empty path takes them all.
        system.connect(bluez, QString(), propertiesInterface, QStringLiteral("PropertiesChanged"), this,
            SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    }
    emit changed();
}

void Bluetooth::onInterfacesAdded(const QDBusObjectPath& path, const QMap<QString, QVariantMap>& interfaces)
{
    const QString p = path.path();
    if (interfaces.contains(adapterInterface) && m_adapter.isEmpty()) {
        const QVariantMap adapter = interfaces[adapterInterface];
        m_adapter = p;
        m_adapterName = adapter[QStringLiteral("Alias")].toString();
        m_powered = adapter[QStringLiteral("Powered")].toBool();
        m_poweredRequest.reported(m_powered);
        m_discovering = adapter[QStringLiteral("Discovering")].toBool();
    }
    if (interfaces.contains(deviceInterface)) {
        QVariantMap device = m_devices.value(p);
        const QVariantMap properties = interfaces[deviceInterface];
        for (auto it = properties.cbegin(); it != properties.cend(); ++it) {
            device[it.key()] = it.value();
        }
        m_devices[p] = device;
    }
    if (interfaces.contains(batteryInterface)) {
        m_devices[p][QStringLiteral("Battery")] = interfaces[batteryInterface][QStringLiteral("Percentage")];
    }
    emit changed();
}

void Bluetooth::onInterfacesRemoved(const QDBusObjectPath& path, const QStringList& interfaces)
{
    if (interfaces.contains(deviceInterface)) {
        m_devices.remove(path.path());
    } else if (interfaces.contains(batteryInterface) && m_devices.contains(path.path())) {
        m_devices[path.path()].remove(QStringLiteral("Battery"));
    }
    if (interfaces.contains(adapterInterface) && path.path() == m_adapter) {
        m_adapter.clear();
    }
    emit changed();
}

void Bluetooth::onPropertiesChanged(const QString& interface, const QVariantMap& changedProperties, const QStringList&)
{
    const QString path = message().path();
    if (interface == adapterInterface && path == m_adapter) {
        m_powered = changedProperties.value(QStringLiteral("Powered"), m_powered).toBool();
        m_poweredRequest.reported(m_powered);
        m_discovering = changedProperties.value(QStringLiteral("Discovering"), m_discovering).toBool();
        m_adapterName = changedProperties.value(QStringLiteral("Alias"), m_adapterName).toString();
    } else if (interface == deviceInterface && m_devices.contains(path)) {
        for (auto it = changedProperties.cbegin(); it != changedProperties.cend(); ++it) {
            m_devices[path][it.key()] = it.value();
        }
    } else if (interface == batteryInterface && m_devices.contains(path)) {
        m_devices[path][QStringLiteral("Battery")] = changedProperties.value(QStringLiteral("Percentage"));
    } else {
        return;
    }
    emit changed();
}

QVariantList Bluetooth::devices(bool paired) const
{
    QVariantList out;
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it) {
        const QVariantMap& d = it.value();
        if (!d[QStringLiteral("Adapter")].value<QDBusObjectPath>().path().isEmpty()
            && d[QStringLiteral("Adapter")].value<QDBusObjectPath>().path() != m_adapter) {
            continue;
        }
        if (d[QStringLiteral("Paired")].toBool() != paired) {
            continue;
        }
        const QString address = d[QStringLiteral("Address")].toString();
        QString name = d[QStringLiteral("Alias")].toString();
        // Without a name (address only): useless in the search.
        if (!paired && (!d.contains(QStringLiteral("Name")) || name.isEmpty())) {
            continue;
        }
        out.append(QVariantMap {
            { QStringLiteral("path"), it.key() },
            { QStringLiteral("address"), address },
            { QStringLiteral("name"), name.isEmpty() ? address : name },
            { QStringLiteral("icon"), d.value(QStringLiteral("Icon"), QStringLiteral("bluetooth")).toString() },
            { QStringLiteral("paired"), paired },
            { QStringLiteral("connected"), d[QStringLiteral("Connected")].toBool() },
            { QStringLiteral("battery"), d.contains(QStringLiteral("Battery")) ? d[QStringLiteral("Battery")].toInt() : -1 },
            { QStringLiteral("busy"), d.value(QStringLiteral("busy")).toBool() },
            { QStringLiteral("rssi"), d.value(QStringLiteral("RSSI"), -200).toInt() },
        });
    }
    // Connected first, then by name; found ones nearest first.
    std::sort(out.begin(), out.end(), [paired](const QVariant& a, const QVariant& b) {
        const QVariantMap x = a.toMap();
        const QVariantMap y = b.toMap();
        if (!paired) {
            return x[QStringLiteral("rssi")].toInt() > y[QStringLiteral("rssi")].toInt();
        }
        if (x[QStringLiteral("connected")] != y[QStringLiteral("connected")]) {
            return x[QStringLiteral("connected")].toBool();
        }
        return x[QStringLiteral("name")].toString().compare(y[QStringLiteral("name")].toString(), Qt::CaseInsensitive) < 0;
    });
    return out;
}

QVariantList Bluetooth::paired() const
{
    return devices(true);
}

QVariantList Bluetooth::found() const
{
    return devices(false);
}

void Bluetooth::fail(const QString& message)
{
    if (!message.isEmpty()) {
        m_error = message;
        emit errorChanged();
    }
}

void Bluetooth::setPowered(bool on)
{
    if (m_adapter.isEmpty()) {
        return;
    }
    m_poweredRequest.ask(on);
    // Even if blocked by rfkill.
    vela::bluetooth::setPowered(
        this, m_adapter, on, [this, on] { return m_poweredRequest.wanted() == on; },
        [this, on] { m_poweredRequest.refused(on); });
}

void Bluetooth::startDiscovery()
{
    if (m_adapter.isEmpty() || m_discovering) {
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(bluez, m_adapter, adapterInterface, QStringLiteral("StartDiscovery"));
    QDBusConnection::systemBus().asyncCall(call);
}

void Bluetooth::stopDiscovery()
{
    if (m_adapter.isEmpty() || !m_discovering) {
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(bluez, m_adapter, adapterInterface, QStringLiteral("StopDiscovery"));
    QDBusConnection::systemBus().asyncCall(call);
}

void Bluetooth::setBusy(const QString& path, bool busy)
{
    if (m_devices.contains(path)) {
        m_devices[path][QStringLiteral("busy")] = busy;
        emit changed();
    }
}

void Bluetooth::callDevice(const QString& path, const QString& method, std::function<void()> then)
{
    m_error.clear();
    emit errorChanged();
    setBusy(path, true);
    QDBusMessage call = QDBusMessage::createMethodCall(bluez, path, deviceInterface, method);
    // Pairing can take up to half a minute (the device must answer).
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call, 60000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, path, then](QDBusPendingCallWatcher* w) {
        const QDBusPendingReply<> reply = *w;
        w->deleteLater();
        if (reply.isError() && !reply.error().name().endsWith(QLatin1String("AlreadyExists"))) {
            setBusy(path, false);
            fail(describe(reply.error()));
            return;
        }
        if (then) {
            then();
        } else {
            setBusy(path, false);
        }
    });
}

void Bluetooth::pairAndConnect(const QString& path)
{
    stopDiscovery();
    callDevice(path, QStringLiteral("Pair"), [this, path] {
        // Trusted: it reconnects by itself the following times.
        QDBusMessage trust = QDBusMessage::createMethodCall(bluez, path, propertiesInterface, QStringLiteral("Set"));
        trust << deviceInterface << QStringLiteral("Trusted") << QVariant::fromValue(QDBusVariant(true));
        QDBusConnection::systemBus().asyncCall(trust);
        callDevice(path, QStringLiteral("Connect"));
    });
}

void Bluetooth::connectDevice(const QString& path)
{
    callDevice(path, QStringLiteral("Connect"));
}

void Bluetooth::disconnectDevice(const QString& path)
{
    callDevice(path, QStringLiteral("Disconnect"));
}

void Bluetooth::removeDevice(const QString& path)
{
    if (m_adapter.isEmpty()) {
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(bluez, m_adapter, adapterInterface, QStringLiteral("RemoveDevice"));
    call << QVariant::fromValue(QDBusObjectPath(path));
    QDBusConnection::systemBus().asyncCall(call);
}
