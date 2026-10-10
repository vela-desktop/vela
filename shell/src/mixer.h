// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPair>
#include <QJsonArray>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class AppModel;

// The sound page of quick settings (Win+Ctrl+V, or the arrow next to the
// volume), like Windows 11's mixer: the outputs to choose from and each app's
// volume and output. Through pactl, like Settings > Sound. It follows the
// system only while the page is open (setActive), on the events SystemStatus
// passes on; the wheel on a taskbar button reads it when needed.
class Mixer : public QObject {
    Q_OBJECT
    // pactl is there (PipeWire or PulseAudio).
    Q_PROPERTY(bool available READ available CONSTANT)
    // [{name, description, icon, isDefault, battery}]: the outputs connected
    // now; battery: the Bluetooth headset's charge, -1 if it doesn't say.
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY changed)
    // [{key, name, icon, desktopId, volume, muted, output, outputDescription}]:
    // the apps playing sound, one per app even if it has several streams (a
    // browser's tabs). output: the output chosen for it, empty if it follows
    // the default one.
    Q_PROPERTY(QVariantList apps READ apps NOTIFY changed)

public:
    explicit Mixer(AppModel* apps, QObject* parent = nullptr);

    bool available() const { return m_available; }
    QVariantList outputs() const { return m_outputs; }
    QVariantList apps() const;

    Q_INVOKABLE void setActive(bool active);
    Q_INVOKABLE void setDefaultOutput(const QString& name);
    // On the 2-point grid, like the system volume (volumesteps.h).
    Q_INVOKABLE void setAppVolume(const QString& key, double volume);
    Q_INVOKABLE void stepAppVolume(const QString& key, int direction);
    Q_INVOKABLE void setAppMuted(const QString& key, bool muted);
    // Where an app's sound goes; empty: the default output, whichever it is.
    // The system remembers it for the next time the app plays (WirePlumber).
    Q_INVOKABLE void setAppOutput(const QString& key, const QString& output);
    // The wheel on a taskbar button: one step for the app with that .desktop
    // file, with the volume indicator. With a stale cache the step is queued
    // until streams arrive; false if the current cache says it isn't playing.
    Q_INVOKABLE bool stepTaskVolume(const QString& desktopId, int direction);

    // pactl subscribe's lines, from SystemStatus.
    void onAudioEvents(const QByteArray& lines);

signals:
    void changed();
    // The indicator for one app's volume (VolumeOsd.qml).
    void appOsdRequested(const QString& key);

private:
    void refresh();
    void load();
    void apply(const QString& defaultName, const QJsonArray& sinks, const QJsonArray& streams,
        const QByteArray& metadata, bool haveMetadata);
    void readBattery(const QString& name, const QString& path, quint64 snapshot);
    QVariantMap* findApp(const QString& key);
    // pactl commands one at a time, in order: a slider dragged quickly can't
    // end on an older value. A command with the same key as a waiting one
    // replaces it.
    void run(const QString& key, const QStringList& arguments);
    void runNext();
    bool busy() const { return m_running || !m_queue.isEmpty(); }

    AppModel* m_appModel;
    bool m_available = false;
    bool m_active = false;
    QVariantList m_outputs;
    QList<QVariantMap> m_apps;
    QElapsedTimer m_loaded; // when the system was last read
    QTimer m_debounce;
    bool m_running = false;
    bool m_loading = false;
    bool m_refreshPending = false;
    quint64 m_revision = 0; // changes invalidate a snapshot being read
    quint64 m_snapshot = 0; // battery replies belong to this output list
    QList<QPair<QString, QStringList>> m_queue;
    QList<QPair<QString, int>> m_taskSteps;
};
