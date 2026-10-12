// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the shell and Settings show and use (docs/audio.md §5.4, §6): the
// model as lists for QML, on the volume's grid and capped at 100%, plus the
// interface's side of every action. A value just set is shown at once,
// until the server reports it or 500 ms pass; a slider being dragged writes
// at most once per event-loop turn. Microphone mute and the default input
// are never shown before the server confirms them.

#include "model.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariant>
#include <QVariantList>

#include <functional>

class QDBusMessage;

namespace vela::audio {

class Core;
class TestSound;

// An app as the desktop knows it (its .desktop file).
struct AppInfo {
    QString desktopId;
    QString name;
    QString icon;
};

class Audio : public QObject {
    Q_OBJECT
    // Connected to PipeWire.
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // [{name, description, icon, volume, muted, isDefault, intervals,
    //   channels, battery, mutePending, muteError, defaultPending,
    //   defaultError, device, stereo, balance}]; battery: a Bluetooth
    //   headset's charge (BlueZ), -1 if it doesn't say; device: its
    //   device's name, empty for virtual ones; stereo: it has a left and a
    //   right side, and balance (-1 left, 0 even, 1 right) means something.
    // Outputs whose jack is unplugged are left out, except the default one.
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY changed)
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY changed)
    // [{key, name, icon, desktopId, playing, recording, volume, muted,
    //   output, outputDescription, input, inputDescription}]: one per app,
    // whatever its number of streams. output/input: the endpoint chosen for
    // it; empty: it follows the default one.
    Q_PROPERTY(QVariantList apps READ apps NOTIFY changed)
    // Sound cards and Bluetooth headsets: [{name, description, icon,
    //   bluetooth, off, profile, profiles: [{index, name, description,
    //   available, priority}], profilePending, profileError, outputs,
    //   inputs}];
    //   profile: the active one's index, -1 unknown; outputs, inputs: their
    //   endpoints' names. Profiles whose ports have nothing plugged in are
    //   left out, except the active one.
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    // The output the test sound is playing on; empty: none.
    Q_PROPERTY(QString testSoundOutput READ testSoundOutput NOTIFY testSoundChanged)
    Q_PROPERTY(QString defaultOutput READ defaultOutput NOTIFY changed)
    Q_PROPERTY(QString defaultInput READ defaultInput NOTIFY changed)
    // The default output, for the volume keys and the taskbar.
    Q_PROPERTY(double volume READ volume NOTIFY changed)
    Q_PROPERTY(bool muted READ muted NOTIFY changed)
    // The default input's mute, as confirmed (the microphone mute key).
    Q_PROPERTY(bool inputMuted READ inputMuted NOTIFY changed)
    // The theme icon for the default output's volume, like Windows' system
    // icon: muted, low, medium, high.
    Q_PROPERTY(QString iconName READ iconName NOTIFY changed)

public:
    using AppResolver = std::function<AppInfo(const AppIdentity&)>;

    explicit Audio(QObject* parent = nullptr);
    ~Audio() override;

    // How an app's raw ids become a name and an icon (the shell's AppModel,
    // Settings' own index). Without one: the stream's own name and icon.
    void setAppResolver(AppResolver resolver);
    Core* core() const { return m_core; }

    bool available() const;
    QVariantList outputs() const { return m_outputs; }
    QVariantList inputs() const { return m_inputs; }
    QVariantList apps() const { return m_apps; }
    QVariantList devices() const { return m_devices; }
    QString testSoundOutput() const;
    QString defaultOutput() const { return m_defaultOutput; }
    QString defaultInput() const { return m_defaultInput; }
    double volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    bool inputMuted() const { return m_inputMuted; }
    QString iconName() const;

    // kind: "output" or "input"; name: the endpoint's node.name.
    Q_INVOKABLE void setDefault(const QString& kind, const QString& name);
    // On the endpoint's grid (volumesteps.h), 0-1.
    Q_INVOKABLE void setVolume(const QString& kind, const QString& name, double volume);
    // One step up (direction > 0) or down; an output is unmuted, like Windows.
    Q_INVOKABLE void stepVolume(const QString& kind, const QString& name, int direction);
    Q_INVOKABLE void setMuted(const QString& kind, const QString& name, bool muted);
    // Left (-1) to right (1); for endpoints with both sides. The volume
    // stays what it is.
    Q_INVOKABLE void setBalance(const QString& kind, const QString& name, double balance);
    // A device's profile (docs/audio.md §6.2), which on a Bluetooth headset
    // is also the codec; "off" turns it off. Shown at once, for up to 3 s.
    Q_INVOKABLE void setProfile(const QString& device, int index);
    // The chime on that output, left then right; again from the start if
    // it is already playing (docs/audio.md §7.3).
    Q_INVOKABLE void playTestSound(const QString& output);
    Q_INVOKABLE void stopTestSound();

    // An app's playback, on the 2-point grid.
    Q_INVOKABLE void setAppVolume(const QString& key, double volume);
    Q_INVOKABLE void stepAppVolume(const QString& key, int direction);
    Q_INVOKABLE void setAppMuted(const QString& key, bool muted);
    // Where an app's sound goes (output) or comes from (input); empty: the
    // default one. WirePlumber remembers it for the next time.
    Q_INVOKABLE void setAppOutput(const QString& key, const QString& output);
    Q_INVOKABLE void setAppInput(const QString& key, const QString& input);
    // Every app follows the defaults again.
    Q_INVOKABLE void resetApps();
    // The key of the app with that .desktop file, if it plays or records.
    Q_INVOKABLE QString appKey(const QString& desktopId) const;

signals:
    void availableChanged();
    void changed();
    void testSoundChanged();

private Q_SLOTS:
    // BlueZ's PropertiesChanged on a headset: its battery.
    void onBluetoothProperties(const QDBusMessage& message);

private:
    // An action waiting for the server. Optimistic ones are shown at once
    // and dropped when confirmed or after 500 ms (a profile: 3 s); the
    // others (privacy) only mark the item as pending, and become an error
    // after 2 s.
    struct Pending {
        QVariant value;
        bool optimistic = true;
        QElapsedTimer since;
        int timeoutMs = 0;
    };

    const Endpoint* endpoint(const QString& kind, const QString& name) const;
    QList<const Stream*> appStreams(const QString& key, Direction direction) const;
    void expect(const QString& key, const QVariant& value, bool optimistic, int timeoutMs = 0);
    // The channels to write for an endpoint: its volume and balance as
    // shown, with what is waiting to be written.
    QList<double> channelsFor(const Endpoint& e, const QString& kind) const;
    void fail(const QString& key);
    void flush();
    void update();
    // A Bluetooth output's battery: read once, then followed.
    void watchBattery(const QString& path);

    Core* m_core;
    AppResolver m_resolver;
    QHash<QString, Pending> m_pending; // "volume output <name>", "mute input <name>"...
    QSet<QString> m_errors;
    QHash<QString, double> m_writes; // volumes and balances waiting for the next loop turn
    QTimer m_flush;
    QTimer m_expiry;
    QHash<QString, int> m_batteries; // BlueZ device path -> percent
    QSet<QString> m_batteryWatched;
    QHash<QString, quint64> m_batteryVersions; // guards late BlueZ replies
    quint64 m_nextBatteryVersion = 0;

    QVariantList m_outputs;
    QVariantList m_inputs;
    QVariantList m_apps;
    QVariantList m_devices;
    TestSound* m_testSound;
    QString m_defaultOutput;
    QString m_defaultInput;
    double m_volume = 0.0;
    bool m_muted = false;
    bool m_inputMuted = false;
    bool m_defaultsKnown = false; // recognize WirePlumber recovery
};

} // namespace vela::audio
