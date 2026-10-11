// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The audio model (docs/audio.md §5): what PipeWire and WirePlumber say,
// nothing more. Volumes are on the scale people see (cubic, like wpctl and
// pactl), 1.0 = 100%.

#include <QList>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace vela::audio {

enum class Direction { Output, Input };

// An output (Audio/Sink) or an input (Audio/Source) node.
struct Endpoint {
    quint32 id = 0;
    quint64 serial = 0;
    QString name; // node.name: stable across restarts
    QString description;
    QString icon;
    Direction direction = Direction::Output;
    QStringList positions; // FL, FR...: the layout
    // One per channel; a suspended node may report fewer (two) until its
    // format is set.
    QList<double> channelVolumes;
    bool muted = false;
    // The device's volume step (the Route's volumeStep, linear); 0: unknown.
    double step = 0.0;
    // False when its route says the jack is unplugged.
    bool available = true;
    bool isDefault = false;
    quint32 device = 0; // 0: none (virtual, null sinks)
    QString bluetoothAddress;
    QString bluezPath;

    // The overall volume: the loudest channel.
    double volume() const
    {
        return channelVolumes.isEmpty() ? 0.0 : *std::max_element(channelVolumes.begin(), channelVolumes.end());
    }
};

// Who a stream belongs to, as the stream says it.
struct AppIdentity {
    QString key; // the binary, else the app name, else node.name
    QString binary; // application.process.binary
    QString name; // application.name
    QString id; // application.id
    QString portalAppId; // pipewire.access.portal.app_id
    QString iconName; // application.icon-name
};

// A playback (Stream/Output/Audio) or recording (Stream/Input/Audio) node.
struct Stream {
    quint32 id = 0;
    quint64 serial = 0;
    Direction direction = Direction::Output;
    AppIdentity app;
    QString nodeName;
    QString mediaRole;
    QList<double> channelVolumes;
    bool muted = false;
    quint32 endpoint = 0; // the endpoint it is linked to; 0: none
    // The endpoint chosen for it (target.object), by node.name; empty: it
    // follows the default one.
    QString target;
    bool running = false;
    bool monitor = false; // stream.monitor: a level meter
    // Recording an output (stream.capture.sink, or linked to a sink's monitor).
    bool capturesOutput = false;
    // Part of a filter (node.link-group) or a virtual node: not an app.
    bool internal = false;

    double volume() const
    {
        return channelVolumes.isEmpty() ? 0.0 : *std::max_element(channelVolumes.begin(), channelVolumes.end());
    }
    // An app's own stream, as the mixers show it.
    bool isApp() const { return !monitor && !capturesOutput && !internal; }
};

struct Snapshot {
    bool connected = false;
    // The "default" metadata is there (WirePlumber is running). Without it
    // the defaults are unknown, not "none".
    bool defaultsKnown = false;
    QString defaultOutput; // node.name
    QString defaultInput;
    QList<Endpoint> endpoints;
    QList<Stream> streams;

    const Endpoint* endpoint(const QString& name) const
    {
        for (const Endpoint& e : endpoints) {
            if (e.name == name) {
                return &e;
            }
        }
        return nullptr;
    }
    const Endpoint* endpoint(quint32 id) const
    {
        for (const Endpoint& e : endpoints) {
            if (e.id == id) {
                return &e;
            }
        }
        return nullptr;
    }
    const Stream* stream(quint32 id) const
    {
        for (const Stream& s : streams) {
            if (s.id == id) {
                return &s;
            }
        }
        return nullptr;
    }
};

// A new overall volume with the balance kept (docs/audio.md §5.3): every
// channel scaled by the same factor, so the loudest one lands on `volume`.
// From silence, `balance` (each channel over the loudest, remembered while
// the volume was up) gives the shape back; without it, all channels equal.
inline QList<double> scaledChannels(const QList<double>& channels, double volume, const QList<double>& balance = {})
{
    volume = std::clamp(volume, 0.0, 1.0);
    QList<double> out;
    const double loudest = channels.isEmpty() ? 0.0 : *std::max_element(channels.begin(), channels.end());
    for (qsizetype i = 0; i < channels.size(); i++) {
        const double shape = loudest > 0.0 ? channels[i] / loudest
            : balance.size() == channels.size() ? balance[i]
                                                : 1.0;
        out.append(std::clamp(shape * volume, 0.0, 1.0));
    }
    return out;
}

// Each channel over the loudest; empty in silence.
inline QList<double> balanceOf(const QList<double>& channels)
{
    QList<double> out;
    const double loudest = channels.isEmpty() ? 0.0 : *std::max_element(channels.begin(), channels.end());
    if (loudest <= 0.0) {
        return out;
    }
    for (double c : channels) {
        out.append(c / loudest);
    }
    return out;
}

} // namespace vela::audio
