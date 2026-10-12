// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the shell and Settings show and use (docs/audio.md §5.4, §6): the
// model as lists for QML, on the volume's grid and capped at 100%, plus the
// interface's side of every action. A value just set is shown at once,
// until the server reports it or 500 ms pass; a slider being dragged writes
// at most once per event-loop turn. Microphone mute and the default input
// are never shown before the server confirms them.

#include "config.h"
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
    //   defaultError, device, stereo, balance, canFormat, rate, format, bits,
    //   rates, formats, fixedRate, fixedFormat, converted}]; battery: a
    //   Bluetooth headset's charge (BlueZ), -1 if it doesn't say; device:
    //   its device's name, empty for virtual ones; stereo: it has a left and
    //   a right side, and balance (-1 left, 0 even, 1 right) means
    //   something.
    //   The format (docs/audio.md §8.1): canFormat: an ALSA device, whose
    //   rates and format can be chosen; rate, format, bits: what it runs at
    //   now (0, "", 0 when it doesn't); rates: the standard rates it can do;
    //   formats: [{name, bits}], one per depth, deepest first; fixedRate:
    //   its only allowed rate, 0 when it follows the system; fixedFormat:
    //   empty for the best; converted: [{name, rate}], the apps playing or
    //   recording on it at another rate.
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
    // Who uses the microphone now (docs/audio.md §7.2): [{key, name, icon,
    //   desktopId}], an app recording from an input (a microphone, or a
    //   virtual source made from one), Settings' level meter included. One
    //   that stops stays about a second, so that a pause doesn't make the
    //   tray icon blink.
    Q_PROPERTY(QVariantList microphoneUsers READ microphoneUsers NOTIFY changed)
    // The graph's sample rate (docs/audio.md §8.1): "automatic" (it follows
    // what plays), "fixed" (sampleRate), "custom" (set elsewhere); empty
    // while unknown.
    Q_PROPERTY(QString sampleRateMode READ sampleRateMode NOTIFY changed)
    Q_PROPERTY(int sampleRate READ sampleRate NOTIFY changed)
    Q_PROPERTY(bool sampleRateError READ sampleRateError NOTIFY changed)
    // The rates the graph can be fixed at (44100 to 192000).
    Q_PROPERTY(QVariantList sampleRates READ sampleRates CONSTANT)
    Q_PROPERTY(QVariantList allowedRates READ allowedRates NOTIFY changed)
    // A WirePlumber or audio restart is waiting for WirePlumber to save its
    // state (docs/audio.md §8).
    Q_PROPERTY(bool restartPending READ restartPending NOTIFY restartPendingChanged)
    Q_PROPERTY(bool restartError READ restartError NOTIFY restartErrorChanged)
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
    Config* config() const { return m_config; }
    // The shell's, once per session: "Automatic" when nobody configured the
    // graph's rates (no Vela file, PipeWire's default) (docs/audio.md §8.1).
    void applyDefaultSampleRate();

    bool available() const;
    QVariantList outputs() const { return m_outputs; }
    QVariantList inputs() const { return m_inputs; }
    QVariantList apps() const { return m_apps; }
    QVariantList devices() const { return m_devices; }
    QVariantList microphoneUsers() const { return m_microphoneUsers; }
    QString sampleRateMode() const { return m_sampleRateMode; }
    int sampleRate() const { return m_sampleRate; }
    bool sampleRateError() const { return m_sampleRateError; }
    QVariantList sampleRates() const;
    QVariantList allowedRates() const { return m_allowedRates; }
    bool restartPending() const;
    bool restartError() const;
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

    // 0: "Automatic"; a rate among sampleRates: always that one
    // (docs/audio.md §8.1). At once, and kept in PipeWire's file for the
    // next sessions.
    Q_INVOKABLE void setSampleRate(int rate);
    // An endpoint's own settings, in WirePlumber's file (docs/audio.md §8),
    // shown at once. A name needs no restart (other programs see it once
    // WirePlumber starts again); a rate or a format restarts WirePlumber
    // (sound stops for about a second). An empty name: the device's own
    // again; rate 0: follow the system; an empty format: the best.
    Q_INVOKABLE void setEndpointName(const QString& kind, const QString& name, const QString& text);
    Q_INVOKABLE void setEndpointRate(const QString& kind, const QString& name, int rate);
    Q_INVOKABLE void setEndpointFormat(const QString& kind, const QString& name, const QString& format);
    // PipeWire, WirePlumber and pipewire-pulse, for when something is stuck.
    Q_INVOKABLE void restartAudio();

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
    void restartPendingChanged();
    void restartErrorChanged();

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
        bool errorOnTimeout = false;
    };

    const Endpoint* endpoint(const QString& kind, const QString& name) const;
    QList<const Stream*> appStreams(const QString& key, Direction direction) const;
    void expect(const QString& key, const QVariant& value, bool optimistic, int timeoutMs = 0,
        bool errorOnTimeout = false);
    // The channels to write for an endpoint: its volume and balance as
    // shown, with what is waiting to be written.
    QList<double> channelsFor(const Endpoint& e, const QString& kind) const;
    void fail(const QString& key);
    void flush();
    void update();
    // How the desktop names a stream's app.
    AppInfo appInfo(const AppIdentity& app) const;
    // The microphone's users, with those that just stopped.
    QVariantList microphoneUsers(const Snapshot& s);
    // The rule to write for an endpoint, with what it offers now.
    DeviceRule ruleFor(const Endpoint& e) const;
    // The names in Vela's rules, shown instead of the node's own
    // (docs/audio.md §8).
    void loadNames();
    QString nameOf(const Endpoint& e) const;
    void writeRule(const QString& key, const DeviceRule& rule, const QVariant& shown);
    // A Bluetooth output's battery: read once, then followed.
    void watchBattery(const QString& path);

    Core* m_core;
    Config* m_config;
    AppResolver m_resolver;
    bool m_applyDefaultSampleRate = false;
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
    QVariantList m_microphoneUsers;
    QString m_sampleRateMode;
    QHash<QString, QString> m_names; // node.name -> the name Vela's rule gives
    int m_sampleRate = 0;
    bool m_sampleRateError = false;
    QVariantList m_allowedRates;
    struct MicrophoneUser {
        QVariantMap info;
        QElapsedTimer since; // since it was last seen recording
    };
    QHash<QString, MicrophoneUser> m_microphoneSeen; // by app key
    QStringList m_microphoneOrder; // first come, first shown
    QTimer m_microphoneHold;
    TestSound* m_testSound;
    QString m_defaultOutput;
    QString m_defaultInput;
    double m_volume = 0.0;
    bool m_muted = false;
    bool m_inputMuted = false;
    bool m_defaultsKnown = false; // recognize WirePlumber recovery
};

} // namespace vela::audio
