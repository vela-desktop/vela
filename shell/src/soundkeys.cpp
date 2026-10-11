// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "soundkeys.h"

#include "audio.h"

SoundKeys::SoundKeys(vela::audio::Audio* audio, QObject* parent)
    : QObject(parent)
    , m_audio(audio)
{
    m_micTimeout.setSingleShot(true);
    m_micTimeout.setInterval(2000); // as long as Audio waits for a confirmation
    connect(&m_micTimeout, &QTimer::timeout, this, [this] {
        m_micInFlight = false;
        emit osdRequested(QStringLiteral("microphone")); // show confirmed state, even on failure
    });
    connect(m_audio, &vela::audio::Audio::changed, this, &SoundKeys::checkMicrophone);
}

void SoundKeys::press(const QString& key)
{
    if (!m_audio->available()) {
        return;
    }
    const QString output = m_audio->defaultOutput();
    if (key == QLatin1String("mic-mute")) {
        const QString input = m_audio->defaultInput();
        if (input.isEmpty()) {
            return;
        }
        if (m_micInFlight) {
            // The displayed state is intentionally not optimistic. Keep the
            // user's final intent, but wait for the first write to settle.
            m_micWanted = !m_micWanted;
            return;
        }
        m_micWanted = !m_audio->inputMuted();
        m_micSent = m_micWanted;
        m_micInFlight = true;
        m_micTimeout.start();
        m_audio->setMuted(QStringLiteral("input"), input, m_micSent);
        return;
    }
    if (output.isEmpty()) {
        return;
    }
    if (key == QLatin1String("mute")) {
        m_audio->setMuted(QStringLiteral("output"), output, !m_audio->muted());
    } else if (key == QLatin1String("up") || key == QLatin1String("down")) {
        m_audio->stepVolume(QStringLiteral("output"), output, key == QLatin1String("up") ? 1 : -1);
    } else {
        return;
    }
    emit osdRequested(QString());
}

bool SoundKeys::stepTaskVolume(const QString& desktopId, int direction)
{
    if (desktopId.isEmpty() || direction == 0) {
        return false;
    }
    // Audio.apps includes recording-only apps, which have no playback
    // volume: do not claim to have changed one or show a misleading OSD.
    for (const QVariant& entry : m_audio->apps()) {
        const QVariantMap app = entry.toMap();
        if (app.value(QStringLiteral("desktopId")).toString() != desktopId
            || !app.value(QStringLiteral("playing")).toBool()) {
            continue;
        }
        const QString key = app.value(QStringLiteral("key")).toString();
        m_audio->stepAppVolume(key, direction);
        emit osdRequested(key);
        return true;
    }
    return false;
}

void SoundKeys::checkMicrophone()
{
    if (!m_micInFlight || m_audio->inputMuted() != m_micSent) {
        return;
    }
    if (m_micWanted != m_micSent) {
        // An intervening press reversed the desired state. Serialize the
        // follow-up write so an older confirmation cannot complete the OSD.
        const QString input = m_audio->defaultInput();
        if (!input.isEmpty()) {
            m_micSent = m_micWanted;
            m_micTimeout.start();
            m_audio->setMuted(QStringLiteral("input"), input, m_micSent);
            return;
        }
    }
    m_micInFlight = false;
    m_micTimeout.stop();
    emit osdRequested(QStringLiteral("microphone"));
}
