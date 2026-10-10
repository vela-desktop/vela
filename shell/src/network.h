// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QProcess>
#include <QVariantList>

// Network & internet, with NetworkManager (nmcli): active connections with
// their addresses, the Wi-Fi networks around, connecting and disconnecting.
// Slow commands (network scans, connecting) run in the background: the page
// never blocks. Used by Settings and by the shell's quick settings (choosing a
// Wi-Fi network).
class Network : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    // [{device, type ("ethernet"/"wifi"), state, connection, ip, gateway, dns,
    // mac}]
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    // [{ssid, signal, secure, active, known}] strongest first
    Q_PROPERTY(QVariantList wifiNetworks READ wifiNetworks NOTIFY wifiNetworksChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    // The outcome of the last connection: "" (in progress or none), "ok", or
    // the error message.
    Q_PROPERTY(QString connectResult READ connectResult NOTIFY connectResultChanged)
    // SSID requiring new credentials after a saved-profile connection fails.
    Q_PROPERTY(QString passwordRequiredSsid READ passwordRequiredSsid NOTIFY connectResultChanged)

public:
    explicit Network(QObject* parent = nullptr);

    bool available() const { return m_available; }
    QVariantList devices() const { return m_devices; }
    QVariantList wifiNetworks() const { return m_wifi; }
    bool scanning() const { return m_scanning; }
    QString connectResult() const { return m_connectResult; }
    QString passwordRequiredSsid() const { return m_passwordRequiredSsid; }

    Q_INVOKABLE void refresh();
    // Wi-Fi networks only (and saved ones), in the background: for the shell's
    // quick settings, which must never stall.
    Q_INVOKABLE void refreshWifi();
    Q_INVOKABLE void scan();
    // Empty password for open or already known networks.
    Q_INVOKABLE void connectWifi(const QString& ssid, const QString& password);
    Q_INVOKABLE void disconnectDevice(const QString& device);
    // The Wi-Fi network in use (the connection has its name).
    Q_INVOKABLE void disconnectWifi(const QString& ssid);
    Q_INVOKABLE void forget(const QString& connection);

signals:
    void devicesChanged();
    void wifiNetworksChanged();
    void scanningChanged();
    void connectResultChanged();

private:
    void readDevices(const QByteArray& output, quint64 revision);
    void finishDevices(quint64 revision);
    void readWifi(const QByteArray& output);
    void refreshAfterAction();

    bool m_available = false;
    QVariantList m_devices;
    QVariantList m_wifi;
    QStringList m_known; // names of the saved connections
    bool m_scanning = false;
    QString m_connectResult;
    QString m_passwordRequiredSsid;
    bool m_detailsRequested = false;
    bool m_loadingDevices = false;
    bool m_devicesPending = false;
    bool m_loadingWifi = false;
    bool m_wifiPending = false;
    quint64 m_revision = 0;
    quint64 m_connectionRevision = 0;
};
