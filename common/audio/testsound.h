// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The test sound of an output's properties (docs/audio.md §7.3): a short
// chime synthesized here, first on the left channel and then on the right
// (once on a mono output), played on that output only, after a moment of
// silence so a waking output doesn't cut its start. If the output goes
// away it stops instead of moving elsewhere.

#include <QObject>
#include <QString>

#include <memory>

namespace vela::audio {

class Core;
struct Endpoint;

class TestSound : public QObject {
    Q_OBJECT

public:
    explicit TestSound(Core* core, QObject* parent = nullptr);
    ~TestSound() override;

    // From the start again if it is already playing. False when it can't
    // (not connected, no such output).
    bool play(const QString& output);
    void stop();
    // The output it is playing on; empty: none.
    QString playing() const { return m_output; }

    // The chime, interleaved float samples at `rate`: on each channel in
    // turn when there are two (left, right), on every channel at once
    // otherwise.
    static QList<float> synthesize(int channels, int rate);

signals:
    void playingChanged();

private:
    struct Playback;

    void finished(quint64 generation);

    Core* m_core;
    std::shared_ptr<Playback> m_playback;
    QString m_output;
    quint64 m_generation = 0;
};

} // namespace vela::audio
