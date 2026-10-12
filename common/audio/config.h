// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Vela's files in WirePlumber's and PipeWire's configuration, and the
// restarts that apply them (docs/audio.md §8). Each file is Vela's own:
// regenerated whole and read back, never any other file next to it.

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace vela::audio {

// One device's rule in WirePlumber's file, matched on node.name.
struct DeviceRule {
    QString node;
    bool bluetooth = false; // monitor.bluez.rules, else monitor.alsa.rules
    QString description; // node.description; empty: the device's own name
    QList<quint32> allowedRates; // audio.allowed-rates; empty: follow the system
    QString format; // audio.format; empty: the best
    // What the device offered before being restricted (EnumFormat then
    // lists only what is allowed): vela.audio.rates, vela.audio.formats.
    QList<quint32> offeredRates;
    QStringList offeredFormats;

    bool restricts() const { return !allowedRates.isEmpty() || !format.isEmpty(); }
    bool isEmpty() const { return description.isEmpty() && !restricts(); }
    bool operator==(const DeviceRule&) const = default;
};

class Config : public QObject {
    Q_OBJECT

public:
    // Restarts a systemd user unit ("wireplumber.service"...). The default
    // asks systemd's user manager over D-Bus.
    using Restarter = std::function<void(const QString& unit)>;

    explicit Config(QObject* parent = nullptr);

    // ~/.config/wireplumber/wireplumber.conf.d/90-vela.conf
    static QString wireplumberFile();
    // ~/.config/pipewire/pipewire.conf.d/90-vela.conf
    static QString pipewireFile();

    // WirePlumber's file.
    QList<DeviceRule> rules() const;
    DeviceRule rule(const QString& node) const;
    // Replaces the node's rule (an empty one goes away), writes the file and
    // restarts WirePlumber (restartWireplumber). False if it can't be
    // written.
    bool setRule(const DeviceRule& rule);

    // PipeWire's file: the graph's allowed rates (docs/audio.md §8.1); empty
    // when there is no file.
    bool hasPipewireFile() const;
    QList<quint32> pipewireRates() const;
    bool setPipewireRates(const QList<quint32>& rates);

    // Vela changed WirePlumber's state (a volume, a default...): WirePlumber
    // saves it about a second later and not when it exits, so a restart
    // waits 1.5 s from here.
    void noteChange();
    // WirePlumber, once 1.5 s have passed since the last change; requests
    // in between become one restart.
    void restartWireplumber();
    // PipeWire, WirePlumber and pipewire-pulse (bound to PipeWire), with
    // the same wait.
    void restartAudio();
    // A restart is waiting for its time.
    bool restartPending() const { return m_restart.isActive(); }

    void setRestarter(Restarter restarter) { m_restarter = std::move(restarter); }

    // SPA-JSON as PipeWire's and WirePlumber's files are written, as
    // QVariant (objects, arrays, numbers, strings); for tests too.
    static QVariant parse(const QByteArray& text);

signals:
    void restartPendingChanged();

private:
    void schedule(const QString& unit);
    void restartNow();

    Restarter m_restarter;
    QElapsedTimer m_lastChange;
    QTimer m_restart;
    QString m_unit; // what the pending restart restarts
};

} // namespace vela::audio
