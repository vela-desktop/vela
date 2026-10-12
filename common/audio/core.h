// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The connection to PipeWire through libwireplumber (docs/audio.md §4, §5):
// it keeps the model up to date and carries out actions. WirePlumber's GLib
// sources run on the default main context, i.e. on Qt's own loop in the
// interface thread; no GLib type appears in this header.

#include "model.h"

#include <QObject>
#include <QTimer>

#include <memory>

struct pw_core;

namespace vela::audio {

class Core : public QObject {
    Q_OBJECT

public:
    explicit Core(QObject* parent = nullptr);
    ~Core() override;

    // False when Qt doesn't run on GLib's main loop (QT_NO_GLIB): then
    // nothing is ever dispatched, and audio stays unavailable.
    static bool eventLoopSupported();

    bool connected() const { return m_snapshot.connected; }
    const Snapshot& snapshot() const { return m_snapshot; }
    // The connection Vela's own streams use (level meters, test sound,
    // docs/audio.md §7); null when not connected. It goes away right after
    // aboutToDisconnect().
    pw_core* pipewire() const;

    // Actions. False when the object doesn't exist, or when not connected.
    // The model changes when the server says so, never before.
    bool setDefault(Direction direction, const QString& name);
    // Per channel, clamped to 0-1.
    bool setChannelVolumes(quint32 node, const QList<double>& volumes);
    // Endpoints keep their balance (scaledChannels); streams get every
    // channel equal. Clamped to 0-1.
    bool setVolume(quint32 node, double volume);
    bool setMuted(quint32 node, bool muted);
    // The endpoint a stream goes to (or comes from), by node.name; empty: the
    // default one again. WirePlumber remembers it for the app.
    bool setTarget(quint32 stream, const QString& endpoint);
    // A device's profile, by index; WirePlumber remembers it.
    bool setProfile(quint32 device, quint32 index);
    // The rates the graph may switch to (PipeWire's "settings" metadata,
    // clock.allowed-rates): applied at once, forgotten when PipeWire
    // restarts (docs/audio.md §8.1).
    bool setAllowedRates(const QList<quint32>& rates);

signals:
    void connectedChanged();
    // The connection is about to be closed: streams made on pipewire() must
    // be destroyed now.
    void aboutToDisconnect();
    // The snapshot has been replaced.
    void changed();

private:
    struct Private;
    friend struct Private;

    void connectNow();
    void teardown();
    void scheduleReconnect();
    void rebuild();

    std::unique_ptr<Private> d;
    Snapshot m_snapshot;
    QTimer m_rebuild; // at most one rebuild per event-loop turn
    QTimer m_reconnect;
    int m_backoff = 0;
};

} // namespace vela::audio
