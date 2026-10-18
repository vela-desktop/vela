// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "requested.h"

#include <QDBusObjectPath>
#include <QObject>
#include <QString>
#include <QVariantMap>

// System status for the taskbar icons and quick settings (Win+A), like Windows
// 11: network (NetworkManager), Bluetooth
// (BlueZ), battery (UPower), power saving profile (power-profiles-daemon).
// Everything updates itself when it changes; what the system lacks is
// "unavailable" and the panel doesn't show it. The volume is vela-audio's
// (common/audio, docs/audio.md), the brightness the compositor's
// (brightness.h, docs/brightness.md).
class SystemStatus : public QObject {
    Q_OBJECT
    // Network
    Q_PROPERTY(bool networkConnected READ networkConnected NOTIFY networkChanged)
    Q_PROPERTY(bool networkWireless READ networkWireless NOTIFY networkChanged)
    Q_PROPERTY(QString networkName READ networkName NOTIFY networkChanged)
    Q_PROPERTY(int wifiStrength READ wifiStrength NOTIFY networkChanged)
    Q_PROPERTY(bool wifiAvailable READ wifiAvailable NOTIFY networkChanged)
    Q_PROPERTY(bool wifiEnabled READ wifiEnabled NOTIFY networkChanged)
    // Bluetooth
    Q_PROPERTY(bool bluetoothAvailable READ bluetoothAvailable NOTIFY bluetoothChanged)
    Q_PROPERTY(bool bluetoothEnabled READ bluetoothEnabled NOTIFY bluetoothChanged)
    // Battery
    Q_PROPERTY(bool batteryPresent READ batteryPresent NOTIFY batteryChanged)
    Q_PROPERTY(int batteryPercent READ batteryPercent NOTIFY batteryChanged)
    Q_PROPERTY(bool batteryCharging READ batteryCharging NOTIFY batteryChanged)
    // Power saving
    Q_PROPERTY(bool powerSaverAvailable READ powerSaverAvailable NOTIFY powerProfileChanged)
    Q_PROPERTY(bool powerSaver READ powerSaver NOTIFY powerProfileChanged)
    // "power-saver", "balanced" or "performance" (if the system offers it)
    Q_PROPERTY(QString powerProfile READ powerProfile WRITE setPowerProfile NOTIFY powerProfileChanged)
    Q_PROPERTY(QStringList powerProfiles READ powerProfiles NOTIFY powerProfileChanged)
    // The theme icon for the network, and airplane mode.
    Q_PROPERTY(QString networkIconName READ networkIcon NOTIFY networkChanged)
    Q_PROPERTY(bool airplane READ airplaneMode NOTIFY airplaneChanged)

public:
    explicit SystemStatus(QObject* parent = nullptr);
    ~SystemStatus() override;

    bool networkConnected() const { return m_networkConnected; }
    bool networkWireless() const { return m_networkWireless; }
    QString networkName() const { return m_networkName; }
    int wifiStrength() const { return m_wifiStrength; }
    bool wifiAvailable() const { return m_wifiAvailable; }
    // What the user asked for until NetworkManager or BlueZ has done it (requested.h).
    bool wifiEnabled() const { return m_wifiRequest.value(m_wifiEnabled); }
    Q_INVOKABLE void setWifiEnabled(bool on);

    bool bluetoothAvailable() const { return !m_adapter.isEmpty(); }
    bool bluetoothEnabled() const { return m_bluetoothRequest.value(m_bluetoothEnabled); }
    Q_INVOKABLE void setBluetoothEnabled(bool on);

    // Airplane mode: no Wi-Fi or Bluetooth.
    Q_INVOKABLE bool airplaneMode() const;
    Q_INVOKABLE void setAirplaneMode(bool on);

    bool batteryPresent() const { return m_batteryPresent; }
    int batteryPercent() const { return m_batteryPercent; }
    bool batteryCharging() const { return m_batteryCharging; }

    bool powerSaverAvailable() const { return m_powerProfilesAvailable; }
    bool powerSaver() const { return m_powerProfile == QLatin1String("power-saver"); }
    Q_INVOKABLE void setPowerSaver(bool on);
    QString powerProfile() const { return m_powerProfile; }
    void setPowerProfile(const QString& profile);
    QStringList powerProfiles() const { return m_powerProfiles; }

    // The theme icon name for the network, like Windows' system icons.
    Q_INVOKABLE QString networkIcon() const;

signals:
    void networkChanged();
    void bluetoothChanged();
    void batteryChanged();
    void powerProfileChanged();
    void airplaneChanged();

private Q_SLOTS:
    void refreshNetwork();
    void refreshBluetooth();
    void refreshBattery();
    void refreshPowerProfile();
    void onPropertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);

private:
    bool m_networkConnected = false;
    bool m_networkWireless = false;
    QString m_networkName;
    int m_wifiStrength = 0;
    bool m_wifiAvailable = false;
    bool m_wifiEnabled = false;
    Requested m_wifiRequest { [this] { emit networkChanged(); } };

    QString m_adapter; // BlueZ adapter path
    bool m_bluetoothEnabled = false;
    Requested m_bluetoothRequest { [this] { emit bluetoothChanged(); } };

    bool m_batteryPresent = false;
    int m_batteryPercent = 0;
    bool m_batteryCharging = false;

    bool m_powerProfilesAvailable = false;
    QString m_powerProfile;
    QStringList m_powerProfiles;
};
