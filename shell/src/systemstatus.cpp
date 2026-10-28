// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "systemstatus.h"

#include "bluetooth/bluetooth.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusVariant>

namespace {

const QString nm = QStringLiteral("org.freedesktop.NetworkManager");
const QString nmPath = QStringLiteral("/org/freedesktop/NetworkManager");
const QString propertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

QVariant dbusGet(const QDBusConnection& bus, const QString& service, const QString& path, const QString& interface,
    const QString& name)
{
    QDBusInterface props(service, path, propertiesInterface, bus);
    const QDBusReply<QDBusVariant> reply = props.call(QStringLiteral("Get"), interface, name);
    return reply.isValid() ? reply.value().variant() : QVariant();
}

void dbusSet(const QDBusConnection& bus, const QString& service, const QString& path, const QString& interface,
    const QString& name, const QVariant& value)
{
    QDBusInterface props(service, path, propertiesInterface, bus);
    props.asyncCall(QStringLiteral("Set"), interface, name, QVariant::fromValue(QDBusVariant(value)));
}

} // namespace

SystemStatus::SystemStatus(vela::bluetooth::Bluetooth* bluetooth, QObject* parent)
    : QObject(parent)
    , m_bluetooth(bluetooth)
{
    QDBusConnection system = QDBusConnection::systemBus();
    connect(this, &SystemStatus::networkChanged, this, &SystemStatus::airplaneChanged);
    connect(m_bluetooth, &vela::bluetooth::Bluetooth::changed, this, &SystemStatus::airplaneChanged);

    // --- Network: NetworkManager.
    system.connect(nm, nmPath, propertiesInterface, QStringLiteral("PropertiesChanged"), this,
        SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    refreshNetwork();

    // --- Battery: UPower's "display" device.
    system.connect(QStringLiteral("org.freedesktop.UPower"), QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice"),
        propertiesInterface, QStringLiteral("PropertiesChanged"), this, SLOT(refreshBattery()));
    refreshBattery();

    // --- Power profiles.
    system.connect(QStringLiteral("org.freedesktop.UPower.PowerProfiles"), QStringLiteral("/org/freedesktop/UPower/PowerProfiles"),
        propertiesInterface, QStringLiteral("PropertiesChanged"), this, SLOT(refreshPowerProfile()));
    refreshPowerProfile();
}

SystemStatus::~SystemStatus() = default;

// ---------------------------------------------------------------- network --

void SystemStatus::onPropertiesChanged(const QString& interface, const QVariantMap&, const QStringList&)
{
    if (interface == nm) {
        refreshNetwork();
    }
}

void SystemStatus::refreshNetwork()
{
    QDBusConnection system = QDBusConnection::systemBus();
    const uint state = dbusGet(system, nm, nmPath, nm, QStringLiteral("State")).toUInt();
    m_networkConnected = state >= 60; // NM_STATE_CONNECTED_SITE and above
    m_wifiEnabled = dbusGet(system, nm, nmPath, nm, QStringLiteral("WirelessEnabled")).toBool();
    m_wifiRequest.reported(m_wifiEnabled);
    m_wifiAvailable = dbusGet(system, nm, nmPath, nm, QStringLiteral("WirelessHardwareEnabled")).toBool();
    m_networkName.clear();
    m_networkWireless = false;
    m_wifiStrength = 0;
    const QDBusObjectPath primary
        = qvariant_cast<QDBusObjectPath>(dbusGet(system, nm, nmPath, nm, QStringLiteral("PrimaryConnection")));
    if (!primary.path().isEmpty() && primary.path() != QLatin1String("/")) {
        const QString active = nm + QStringLiteral(".Connection.Active");
        m_networkName = dbusGet(system, nm, primary.path(), active, QStringLiteral("Id")).toString();
        const QString type = dbusGet(system, nm, primary.path(), active, QStringLiteral("Type")).toString();
        m_networkWireless = type == QLatin1String("802-11-wireless");
        if (m_networkWireless) {
            const QDBusObjectPath ap
                = qvariant_cast<QDBusObjectPath>(dbusGet(system, nm, primary.path(), active, QStringLiteral("SpecificObject")));
            m_wifiStrength = dbusGet(system, nm, ap.path(), nm + QStringLiteral(".AccessPoint"), QStringLiteral("Strength")).toInt();
        }
    }
    emit networkChanged();
}

void SystemStatus::setWifiEnabled(bool on)
{
    const Requested::Id request = m_wifiRequest.ask(on);
    QDBusMessage call = QDBusMessage::createMethodCall(nm, nmPath, propertiesInterface, QStringLiteral("Set"));
    call << nm << QStringLiteral("WirelessEnabled") << QVariant::fromValue(QDBusVariant(on));
    auto* reply = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call), this);
    connect(reply, &QDBusPendingCallWatcher::finished, this, [this, request](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        if (w->isError()) {
            m_wifiRequest.refused(request);
        }
    });
}

QString SystemStatus::networkIcon() const
{
    if (!m_networkConnected) {
        return QStringLiteral("network-offline");
    }
    if (!m_networkWireless) {
        return QStringLiteral("network-wired");
    }
    return m_wifiStrength > 75 ? QStringLiteral("network-wireless-signal-excellent")
        : m_wifiStrength > 50  ? QStringLiteral("network-wireless-signal-good")
        : m_wifiStrength > 25  ? QStringLiteral("network-wireless-signal-ok")
                               : QStringLiteral("network-wireless-signal-weak");
}

// --------------------------------------------------------- airplane mode --

// Bluetooth counts when there is an adapter, or a blocked platform switch
// that turning it on unblocks (docs/bluetooth.md §6.3).
bool SystemStatus::airplaneMode() const
{
    const bool bluetooth = m_bluetooth->available() || m_bluetooth->platformBlocked();
    return (!m_wifiAvailable || !wifiEnabled()) && (!bluetooth || !m_bluetooth->powered()) && (m_wifiAvailable || bluetooth);
}

void SystemStatus::setAirplaneMode(bool on)
{
    if (m_wifiAvailable) {
        setWifiEnabled(!on);
    }
    if (m_bluetooth->powered() == on) {
        m_bluetooth->setPowered(!on);
    }
}

// ----------------------------------------------------------------- battery --

void SystemStatus::refreshBattery()
{
    QDBusConnection system = QDBusConnection::systemBus();
    const QString service = QStringLiteral("org.freedesktop.UPower");
    const QString path = QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice");
    const QString device = QStringLiteral("org.freedesktop.UPower.Device");
    // Type 2: battery (a desktop's DisplayDevice says "not present").
    m_batteryPresent = dbusGet(system, service, path, device, QStringLiteral("IsPresent")).toBool()
        && dbusGet(system, service, path, device, QStringLiteral("Type")).toUInt() == 2;
    m_batteryPercent = int(std::lround(dbusGet(system, service, path, device, QStringLiteral("Percentage")).toDouble()));
    const uint state = dbusGet(system, service, path, device, QStringLiteral("State")).toUInt();
    m_batteryCharging = state == 1 || state == 4; // charging, charged
    emit batteryChanged();
}

// ---------------------------------------------------------- power profiles --

void SystemStatus::refreshPowerProfile()
{
    const QVariant profile = dbusGet(QDBusConnection::systemBus(), QStringLiteral("org.freedesktop.UPower.PowerProfiles"),
        QStringLiteral("/org/freedesktop/UPower/PowerProfiles"), QStringLiteral("org.freedesktop.UPower.PowerProfiles"),
        QStringLiteral("ActiveProfile"));
    m_powerProfilesAvailable = profile.isValid();
    m_powerProfile = profile.toString();
    // The offered profiles: aa{sv}, each with the "Profile" key.
    m_powerProfiles.clear();
    if (m_powerProfilesAvailable) {
        const QVariant profiles = dbusGet(QDBusConnection::systemBus(), QStringLiteral("org.freedesktop.UPower.PowerProfiles"),
            QStringLiteral("/org/freedesktop/UPower/PowerProfiles"), QStringLiteral("org.freedesktop.UPower.PowerProfiles"),
            QStringLiteral("Profiles"));
        const QDBusArgument list = profiles.value<QDBusArgument>();
        if (list.currentType() == QDBusArgument::ArrayType) {
            list.beginArray();
            while (!list.atEnd()) {
                QVariantMap entry;
                list >> entry;
                m_powerProfiles << entry.value(QStringLiteral("Profile")).toString();
            }
            list.endArray();
        }
    }
    emit powerProfileChanged();
}

void SystemStatus::setPowerSaver(bool on)
{
    setPowerProfile(on ? QStringLiteral("power-saver") : QStringLiteral("balanced"));
}

void SystemStatus::setPowerProfile(const QString& profile)
{
    dbusSet(QDBusConnection::systemBus(), QStringLiteral("org.freedesktop.UPower.PowerProfiles"),
        QStringLiteral("/org/freedesktop/UPower/PowerProfiles"), QStringLiteral("org.freedesktop.UPower.PowerProfiles"),
        QStringLiteral("ActiveProfile"), profile);
}
