// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A level meter (docs/audio.md §7.1): the bar that moves with the sound of
// an input, an output or an app. While active it records peaks through
// PipeWire's own monitor streams (stream.monitor, resample.peaks), on the
// audio core's connection; the samples are read on PipeWire's real-time
// thread, never on the interface's. For QML: LevelMeter { audio: Audio;
// kind: "input"; name: Audio.defaultInput; active: visible }.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <memory>

namespace vela::audio {

class Audio;

class LevelMeter : public QObject {
    Q_OBJECT
    Q_PROPERTY(vela::audio::Audio* audio READ audio WRITE setAudio NOTIFY audioChanged)
    // "input": what an input hears; "output": what an output plays; "app":
    // what an app plays, all its streams together.
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY kindChanged)
    // The endpoint's node.name, or the app's key.
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    // Streams exist only while active: a page or a mixer that is visible.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // 0-1 on the volume's scale (cubic), with a short decay like Windows.
    Q_PROPERTY(double level READ level NOTIFY levelChanged)

public:
    explicit LevelMeter(QObject* parent = nullptr);
    ~LevelMeter() override;

    Audio* audio() const { return m_audio; }
    void setAudio(Audio* audio);
    QString kind() const { return m_kind; }
    void setKind(const QString& kind);
    QString name() const { return m_name; }
    void setName(const QString& name);
    bool active() const { return m_active; }
    void setActive(bool active);
    double level() const { return m_level; }

    // The node ids being recorded, for the tests.
    QList<quint32> monitored() const { return m_monitors.keys(); }

signals:
    void audioChanged();
    void kindChanged();
    void nameChanged();
    void activeChanged();
    void levelChanged();

private:
    struct Monitor;

    // The streams match what should be recorded now.
    void reconcile();
    void closeAll();
    void tick();

    QPointer<Audio> m_audio;
    QString m_kind = QStringLiteral("input");
    QString m_name;
    bool m_active = false;
    double m_level = 0.0;
    QHash<quint32, std::shared_ptr<Monitor>> m_monitors; // by the recorded node
    QHash<quint32, quint64> m_failed; // node id -> serial, no stale failures on id reuse
    QTimer m_retry;
    QTimer m_tick;
    QTimer m_reconcile;
};

} // namespace vela::audio
