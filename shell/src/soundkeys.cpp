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
        emit osdRequested(QStringLiteral("microphone")); // unchanged: it didn't work
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
        m_micWanted = !m_audio->inputMuted();
        m_audio->setMuted(QStringLiteral("input"), input, m_micWanted);
        m_micTimeout.start();
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
    const QString key = m_audio->appKey(desktopId);
    if (key.isEmpty() || direction == 0) {
        return false;
    }
    m_audio->stepAppVolume(key, direction);
    emit osdRequested(key);
    return true;
}

void SoundKeys::checkMicrophone()
{
    if (m_micTimeout.isActive() && m_audio->inputMuted() == m_micWanted) {
        m_micTimeout.stop();
        emit osdRequested(QStringLiteral("microphone"));
    }
}
