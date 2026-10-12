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

// The standard sample rates Vela offers (docs/audio.md §8.1): the graph's
// with "Automatic", a device's fixed one among those it can do.
inline const QList<quint32>& standardRates()
{
    static const QList<quint32> rates { 44100, 48000, 88200, 96000, 176400, 192000 };
    return rates;
}

// A list as SPA-JSON writes it ("[ 44100 48000 ]", "[ \"S16LE\" ]") and
// PipeWire's properties and metadata carry it.
inline QStringList parseList(const QString& text)
{
    QString list = text.trimmed();
    if (list.startsWith(u'[')) {
        list.remove(0, 1);
    }
    if (list.endsWith(u']')) {
        list.chop(1);
    }
    list.replace(u',', u' ').remove(u'"');
    return list.split(u' ', Qt::SkipEmptyParts);
}

inline QList<quint32> parseRates(const QString& text)
{
    QList<quint32> out;
    for (const QString& item : parseList(text)) {
        bool ok = false;
        const quint32 rate = item.toUInt(&ok);
        if (ok && rate > 0 && !out.contains(rate)) {
            out.append(rate);
        }
    }
    return out;
}

// A sample format's bits ("S24_32LE" -> 24, "F32P" -> 32); 0: unknown.
inline int formatBits(const QString& format)
{
    if (format.startsWith(QLatin1String("S24"))) {
        return 24;
    }
    if (format.startsWith(QLatin1String("S16")) || format.startsWith(QLatin1String("U16"))) {
        return 16;
    }
    if (format.startsWith(QLatin1String("S32")) || format.startsWith(QLatin1String("F32"))
        || format.startsWith(QLatin1String("U32"))) {
        return 32;
    }
    if (format.startsWith(QLatin1String("F64"))) {
        return 64;
    }
    if (format.startsWith(QLatin1String("S8")) || format.startsWith(QLatin1String("U8"))) {
        return 8;
    }
    return 0;
}

// One of a device's profiles: what it is set up for (stereo output, HDMI,
// a Bluetooth codec, call mode, off).
struct Profile {
    quint32 index = 0;
    QString name; // "off", "output:analog-stereo", "a2dp-sink-ldac"...
    QString description;
    // False when what it needs isn't there (an HDMI port with no display).
    bool available = true;
    quint32 priority = 0;
};

// A sound card or a Bluetooth headset (Audio/Device).
struct Device {
    quint32 id = 0;
    QString name; // device.name: stable across restarts
    QString description;
    QString icon;
    bool bluetooth = false;
    QList<Profile> profiles;
    qint32 profile = -1; // the active one's index; -1: unknown

    const Profile* activeProfile() const
    {
        for (const Profile& p : profiles) {
            if (qint32(p.index) == profile) {
                return &p;
            }
        }
        return nullptr;
    }
    // Turned off: its "off" profile is the active one.
    bool off() const
    {
        const Profile* p = activeProfile();
        return p && p->name == QLatin1String("off");
    }
};

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
    // The position of each of channelVolumes.
    QStringList volumePositions;
    // Each channel over the loudest, from the last time the volume was up:
    // from silence the volume comes back with the same balance.
    QList<double> shape;
    bool muted = false;
    // The device's volume step (the Route's volumeStep, linear); 0: unknown.
    double step = 0.0;
    // False when its route says the jack is unplugged.
    bool available = true;
    bool isDefault = false;
    quint32 device = 0; // 0: none (virtual, null sinks)
    QString bluetoothAddress;
    QString bluezPath;
    // An ALSA node (device.api): its format can be chosen (docs/audio.md
    // §8.1). Bluetooth's is the codec's.
    bool alsa = false;
    // What it runs at now (its Format); 0 and empty when it isn't running.
    quint32 rate = 0;
    QString format; // "S24_32LE"...
    // What it can run at: the standard rates in its range and its sample
    // formats (EnumFormat), or what it offered before Vela restricted it
    // (vela.audio.rates, vela.audio.formats).
    QList<quint32> rates;
    QStringList formats;
    // Its restrictions (audio.allowed-rates, audio.format), whoever set them.
    QList<quint32> allowedRates;
    QString fixedFormat;

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
    // Its sample rate (Format); 0: not negotiated.
    quint32 rate = 0;
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
    // PipeWire's "settings" metadata is there: the graph's rates are known.
    bool settingsKnown = false;
    quint32 clockRate = 0; // clock.rate, the default one
    QList<quint32> allowedRates; // clock.allowed-rates
    QList<Device> devices;
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
    const Device* device(quint32 id) const
    {
        for (const Device& d : devices) {
            if (d.id == id) {
                return &d;
            }
        }
        return nullptr;
    }
    const Device* device(const QString& name) const
    {
        for (const Device& d : devices) {
            if (d.name == name) {
                return &d;
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

// Which side a channel is on: -1 left, 1 right, 0 neither (center, LFE,
// mono, aux), as PulseAudio's balance counts them.
inline int sideOf(const QString& position)
{
    static const QStringList left { QStringLiteral("FL"), QStringLiteral("RL"), QStringLiteral("SL"),
        QStringLiteral("FLC"), QStringLiteral("RLC"), QStringLiteral("FLW"), QStringLiteral("FLH"),
        QStringLiteral("TFL"), QStringLiteral("TRL"), QStringLiteral("TSL"), QStringLiteral("TFLC"),
        QStringLiteral("LLFE") };
    static const QStringList right { QStringLiteral("FR"), QStringLiteral("RR"), QStringLiteral("SR"),
        QStringLiteral("FRC"), QStringLiteral("RRC"), QStringLiteral("FRW"), QStringLiteral("FRH"),
        QStringLiteral("TFR"), QStringLiteral("TRR"), QStringLiteral("TSR"), QStringLiteral("TFRC"),
        QStringLiteral("RLFE") };
    return left.contains(position) ? -1 : right.contains(position) ? 1 : 0;
}

// Channels on both sides: the balance can be set.
inline bool hasStereo(const QStringList& positions)
{
    bool l = false;
    bool r = false;
    for (const QString& p : positions) {
        l = l || sideOf(p) < 0;
        r = r || sideOf(p) > 0;
    }
    return l && r;
}

// The left/right balance (docs/audio.md §6.2), as PulseAudio computes it:
// -1 only the left side plays, 1 only the right, 0 both alike.
inline double stereoBalance(const QStringList& positions, const QList<double>& channels)
{
    double left = 0.0;
    double right = 0.0;
    for (qsizetype i = 0; i < channels.size(); i++) {
        const int side = sideOf(positions.value(i));
        if (side < 0) {
            left = std::max(left, channels[i]);
        } else if (side > 0) {
            right = std::max(right, channels[i]);
        }
    }
    if (left == right) {
        return 0.0;
    }
    return left > right ? -(1.0 - right / left) : 1.0 - left / right;
}

// The channels with a new balance: the louder side keeps its level, the
// other one goes down; center and LFE channels are left alone.
inline QList<double> withStereoBalance(const QStringList& positions, const QList<double>& channels, double balance)
{
    balance = std::clamp(balance, -1.0, 1.0);
    double left = 0.0;
    double right = 0.0;
    for (qsizetype i = 0; i < channels.size(); i++) {
        const int side = sideOf(positions.value(i));
        if (side < 0) {
            left = std::max(left, channels[i]);
        } else if (side > 0) {
            right = std::max(right, channels[i]);
        }
    }
    const double loudest = std::max(left, right);
    const double newLeft = balance <= 0.0 ? loudest : (1.0 - balance) * loudest;
    const double newRight = balance >= 0.0 ? loudest : (1.0 + balance) * loudest;
    QList<double> out = channels;
    for (qsizetype i = 0; i < out.size(); i++) {
        const int side = sideOf(positions.value(i));
        if (side == 0) {
            continue;
        }
        const double from = side < 0 ? left : right;
        const double to = side < 0 ? newLeft : newRight;
        out[i] = std::clamp(from > 0.0 ? out[i] * to / from : to, 0.0, 1.0);
    }
    return out;
}

} // namespace vela::audio
