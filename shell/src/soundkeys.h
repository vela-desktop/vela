// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The shell's own side of the sound (docs/audio.md §6.1): the keyboard's
// volume and microphone keys, the knob, and the wheel on the taskbar, each
// with the indicator (VolumeOsd.qml). The rest is vela-audio's Audio.

#include <QObject>
#include <QTimer>

namespace vela::audio {
class Audio;
}

class SoundKeys : public QObject {
    Q_OBJECT

public:
    explicit SoundKeys(vela::audio::Audio* audio, QObject* parent = nullptr);

    // "up", "down", "mute" (toggles the default output), "mic-mute" (toggles
    // the default input).
    Q_INVOKABLE void press(const QString& key);
    // The wheel on a taskbar button: one step for the app with that .desktop
    // file, with the indicator; false if it plays nothing.
    Q_INVOKABLE bool stepTaskVolume(const QString& desktopId, int direction);

signals:
    // The indicator: "" for the system volume, "microphone", or an app's key.
    void osdRequested(const QString& target);

private:
    void checkMicrophone();

    vela::audio::Audio* m_audio;
    // The microphone's indicator waits for the server: a privacy-related
    // state is never shown before it is true.
    bool m_micWanted = false; // latest intent, including repeated key presses
    bool m_micSent = false; // the pending server write
    bool m_micInFlight = false;
    QTimer m_micTimeout;
};
