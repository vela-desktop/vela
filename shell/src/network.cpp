// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "network.h"
#include "asyncprocess.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace {

// nmcli -t separates fields with ':' and writes "\:" for those inside values.
QStringList splitTerse(const QString& line)
{
    QStringList fields;
    QString field;
    for (qsizetype i = 0; i < line.size(); ++i) {
        if (line[i] == u'\\' && i + 1 < line.size()) {
            field += line[++i];
        } else if (line[i] == u':') {
            fields << field;
            field.clear();
        } else {
            field += line[i];
        }
    }
    fields << field;
    return fields;
}

bool validTerse(const QByteArray& output, qsizetype fields)
{
    for (const QString& line : QString::fromUtf8(output).split(u'\n', Qt::SkipEmptyParts)) {
        if (splitTerse(line).size() < fields) {
            return false;
        }
    }
    return true;
}

} // namespace

Network::Network(QObject* parent)
    : QObject(parent)
    , m_available(!QStandardPaths::findExecutable(QStringLiteral("nmcli")).isEmpty())
{
    // Read when the page opens (refresh()), not at startup.
}

void Network::refresh()
{
    if (!m_available) {
        return;
    }
    m_detailsRequested = true;
    refreshWifi();
    if (m_loadingDevices) {
        m_devicesPending = true;
        return;
    }
    m_loadingDevices = true;
    m_devicesPending = false;
    const quint64 revision = m_revision;
    vela::runProcess(this, QStringLiteral("nmcli"),
        { QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("DEVICE,TYPE,STATE,CONNECTION"), QStringLiteral("device") },
        3000, [this, revision](vela::ProcessResult result) {
            if (result.ok && revision == m_revision && validTerse(result.output, 4)) {
                readDevices(result.output, revision);
            } else {
                finishDevices(revision);
            }
        });
}

void Network::readDevices(const QByteArray& output, quint64 revision)
{
    QVariantList devices;
    QList<vela::ProcessQuery> queries;
    const QStringList lines = QString::fromUtf8(output).split(u'\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QStringList f = splitTerse(line);
        if (f.size() < 4 || (f[1] != QLatin1String("ethernet") && f[1] != QLatin1String("wifi"))) {
            continue;
        }
        devices.append(QVariantMap { { QStringLiteral("device"), f[0] }, { QStringLiteral("type"), f[1] },
            { QStringLiteral("state"), f[2] }, { QStringLiteral("connection"), f[3] } });
        queries.append({ f[0], QStringLiteral("nmcli"),
            { QStringLiteral("-t"), QStringLiteral("-f"),
                QStringLiteral("GENERAL.HWADDR,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS,IP6.ADDRESS"),
                QStringLiteral("device"), QStringLiteral("show"), f[0] }, 3000 });
    }
    vela::queryProcesses(this, queries,
        [this, revision, devices](bool ok, const QMap<QString, QByteArray>& results) mutable {
            for (QVariant& entry : devices) {
                QVariantMap device = entry.toMap();
                const QByteArray details = results.value(device.value(QStringLiteral("device")).toString());
                ok = ok && validTerse(details, 2);
                QStringList dns;
                QStringList ipv6;
                for (const QString& detail : QString::fromUtf8(details).split(u'\n', Qt::SkipEmptyParts)) {
                    const QStringList fields = splitTerse(detail);
                    if (fields.size() < 2) {
                        continue;
                    }
                    const QString key = fields[0].section(u'[', 0, 0);
                    const QString value = fields[1];
                    if (key == QLatin1String("GENERAL.HWADDR")) {
                        device[QStringLiteral("mac")] = value;
                    } else if (key == QLatin1String("IP4.ADDRESS") && !device.contains(QStringLiteral("ip"))) {
                        device[QStringLiteral("ip")] = value.section(u'/', 0, 0);
                    } else if (key == QLatin1String("IP4.GATEWAY")) {
                        device[QStringLiteral("gateway")] = value;
                    } else if (key == QLatin1String("IP4.DNS")) {
                        dns << value;
                    } else if (key == QLatin1String("IP6.ADDRESS")) {
                        ipv6 << value.section(u'/', 0, 0);
                    }
                }
                device[QStringLiteral("dns")] = dns.join(QStringLiteral(", "));
                device[QStringLiteral("ipv6")] = ipv6.join(QStringLiteral(", "));
                entry = device;
            }
            if (ok && revision == m_revision) {
                m_devices = devices;
                emit devicesChanged();
            }
            finishDevices(revision);
        });
}

void Network::finishDevices(quint64 revision)
{
    m_loadingDevices = false;
    if (std::exchange(m_devicesPending, false) || revision != m_revision) {
        refresh();
    }
}

void Network::refreshWifi()
{
    if (!m_available) {
        return;
    }
    // A hardware scan may be slow. Read NM's cached AP list concurrently
    // instead of hiding it until the scan completes.
    if (m_loadingWifi) {
        m_wifiPending = true;
        return;
    }
    m_loadingWifi = true;
    m_wifiPending = false;
    const quint64 revision = m_revision;
    const QString nmcli = QStringLiteral("nmcli");
    vela::queryProcesses(this,
        { { QStringLiteral("known"), nmcli, { QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("NAME,TYPE"), QStringLiteral("connection") }, 3000 },
            { QStringLiteral("wifi"), nmcli,
                { QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("IN-USE,SSID,SIGNAL,SECURITY"),
                    QStringLiteral("device"), QStringLiteral("wifi"), QStringLiteral("list"), QStringLiteral("--rescan"), QStringLiteral("no") }, 3000 } },
        [this, revision](bool ok, const QMap<QString, QByteArray>& results) {
            m_loadingWifi = false;
            const QByteArray known = results.value(QStringLiteral("known"));
            const QByteArray wifi = results.value(QStringLiteral("wifi"));
            if (ok && revision == m_revision && validTerse(known, 2) && validTerse(wifi, 4)) {
                m_known.clear();
                for (const QString& line : QString::fromUtf8(known).split(u'\n', Qt::SkipEmptyParts)) {
                    const QStringList f = splitTerse(line);
                    if (f.size() >= 2 && f[1].contains(QLatin1String("wireless"))) {
                        m_known << f[0];
                    }
                }
                readWifi(wifi);
            }
            if (std::exchange(m_wifiPending, false) || revision != m_revision) {
                refreshWifi();
            }
        });
}

void Network::readWifi(const QByteArray& output)
{
    QVariantList networks;
    QStringList seen;
    for (const QString& line : QString::fromUtf8(output).split(u'\n', Qt::SkipEmptyParts)) {
        const QStringList f = splitTerse(line);
        if (f.size() < 4 || f[1].isEmpty()) {
            continue; // hidden networks
        }
        const bool active = f[0] == QLatin1String("*");
        // The same network from several access points: a single entry (the
        // strongest first).
        const qsizetype index = seen.indexOf(f[1]);
        if (index >= 0) {
            if (active) {
                QVariantMap entry = networks[index].toMap();
                entry[QStringLiteral("active")] = true;
                networks[index] = entry;
            }
            continue;
        }
        seen << f[1];
        networks.append(QVariantMap {
            { QStringLiteral("ssid"), f[1] },
            { QStringLiteral("signal"), f[2].toInt() },
            { QStringLiteral("secure"), !f[3].isEmpty() && f[3] != QLatin1String("--") },
            { QStringLiteral("active"), active },
            { QStringLiteral("known"), m_known.contains(f[1]) },
        });
    }
    // The connected one on top, then strongest first.
    std::stable_sort(networks.begin(), networks.end(), [](const QVariant& a, const QVariant& b) {
        const QVariantMap x = a.toMap();
        const QVariantMap y = b.toMap();
        if (x[QStringLiteral("active")] != y[QStringLiteral("active")]) {
            return x[QStringLiteral("active")].toBool();
        }
        return x[QStringLiteral("signal")].toInt() > y[QStringLiteral("signal")].toInt();
    });
    m_wifi = networks;
    emit wifiNetworksChanged();
}

void Network::scan()
{
    if (!m_available || m_scanning) {
        return;
    }
    m_scanning = true;
    const quint64 revision = ++m_revision;
    emit scanningChanged();
    refreshWifi(); // show cached access points while hardware discovery runs
    vela::runProcess(this, QStringLiteral("nmcli"),
        { QStringLiteral("-t"), QStringLiteral("-f"), QStringLiteral("IN-USE,SSID,SIGNAL,SECURITY"), QStringLiteral("device"),
            QStringLiteral("wifi"), QStringLiteral("list"), QStringLiteral("--rescan"), QStringLiteral("yes") },
        30000, [this, revision](vela::ProcessResult result) {
            if (result.ok && revision == m_revision && validTerse(result.output, 4)) {
                readWifi(result.output);
            }
            m_scanning = false;
            emit scanningChanged();
            refreshWifi();
        });
}

void Network::connectWifi(const QString& ssid, const QString& password)
{
    ++m_revision;
    const quint64 connection = ++m_connectionRevision;
    m_connectResult.clear();
    m_passwordRequiredSsid.clear();
    emit connectResultChanged();

    // A saved connection already owns the Wi-Fi credentials. Re-activate it
    // instead of using 'device wifi connect', which may request a new PSK
    // without an interactive secret agent (or create a duplicate profile).
    const bool saved = m_known.contains(ssid) && password.isEmpty();
    QStringList arguments = saved
        ? QStringList { QStringLiteral("connection"), QStringLiteral("up"), QStringLiteral("id"), ssid }
        : QStringList { QStringLiteral("device"), QStringLiteral("wifi"), QStringLiteral("connect"), ssid };
    if (!password.isEmpty()) {
        arguments << QStringLiteral("password") << password;
    }
    vela::runProcess(this, QStringLiteral("nmcli"), arguments, 90000,
        [this, connection, ssid, saved](vela::ProcessResult result) {
            if (connection != m_connectionRevision) {
                return;
            }
            if (result.ok) {
                m_connectResult = QStringLiteral("ok");
            } else {
                // nmcli's stderr is localized, implementation-specific and
                // frequently contains internal key names. Never show it raw.
                const QString details = QString::fromUtf8(result.error).toLower();
                const bool credentials = details.contains(QLatin1String("psk"))
                    || details.contains(QLatin1String("password"))
                    || details.contains(QLatin1String("passwd-file"))
                    || details.contains(QLatin1String("secret"))
                    || details.contains(QLatin1String("segreti"))
                    || details.contains(QLatin1String("autentic"))
                    || details.contains(QLatin1String("authentic"));
                if (credentials) {
                    m_passwordRequiredSsid = ssid;
                    m_connectResult = QCoreApplication::translate("Network", "Password required or incorrect. Enter the Wi-Fi password again.");
                } else if (details.contains(QLatin1String("timed out"))
                    || details.contains(QLatin1String("timeout"))
                    || details.contains(QLatin1String("tempo scaduto"))) {
                    m_connectResult = QCoreApplication::translate("Network", "Connection timed out. Please try again.");
                } else if (saved) {
                    // Saved profiles without usable secrets need a manual
                    // retry even if NetworkManager returns an opaque error.
                    m_passwordRequiredSsid = ssid;
                    m_connectResult = QCoreApplication::translate("Network", "Couldn't reconnect. Re-enter the Wi-Fi password or try again.");
                } else {
                    m_connectResult = QCoreApplication::translate("Network", "Couldn't connect to this network. Please try again.");
                }
            }
            emit connectResultChanged();
            refreshAfterAction();
        });
}

void Network::disconnectDevice(const QString& device)
{
    ++m_revision;
    vela::runProcess(this, QStringLiteral("nmcli"), { QStringLiteral("device"), QStringLiteral("disconnect"), device },
        30000, [this](vela::ProcessResult) { refreshAfterAction(); });
}

void Network::disconnectWifi(const QString& ssid)
{
    ++m_revision;
    vela::runProcess(this, QStringLiteral("nmcli"), { QStringLiteral("connection"), QStringLiteral("down"), QStringLiteral("id"), ssid },
        30000, [this](vela::ProcessResult) { refreshAfterAction(); });
}

void Network::forget(const QString& connection)
{
    ++m_revision;
    vela::runProcess(this, QStringLiteral("nmcli"), { QStringLiteral("connection"), QStringLiteral("delete"), QStringLiteral("id"), connection },
        30000, [this](vela::ProcessResult) { refreshAfterAction(); });
}

void Network::refreshAfterAction()
{
    ++m_revision;
    if (m_detailsRequested) {
        refresh();
    } else {
        refreshWifi();
    }
}
