// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QPair>
#include <QSet>
#include <QTimer>
#include <QVariantList>

class QJsonArray;
class QProcess;

// Audio outputs and inputs for System > Sound, from PipeWire or PulseAudio
// through pactl (the same the taskbar uses). Updates itself when something
// changes (pactl subscribe).
class Audio : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    // [{name, description, volume (0-1.5), muted, isDefault}]
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY changed)
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY changed)

public:
    explicit Audio(QObject* parent = nullptr);

    bool available() const { return m_available; }
    QVariantList outputs() const { return m_outputs; }
    QVariantList inputs() const { return m_inputs; }

    Q_INVOKABLE void refresh();
    // kind: "output" or "input"
    Q_INVOKABLE void setDefault(const QString& kind, const QString& name);
    Q_INVOKABLE void setVolume(const QString& kind, const QString& name, double volume);
    Q_INVOKABLE void setMuted(const QString& kind, const QString& name, bool muted);

signals:
    void changed();

private:
    QVariantList list(const QJsonArray& devices, bool inputs, const QString& defaultName) const;
    void run(const QString& key, const QStringList& arguments);
    void markMute(const QString& kind, const QString& name, bool pending, bool error);
    void markDefault(const QString& kind, bool pending, bool error);
    void runNext();
    void update(const QString& kind, const QString& name, const QString& property, const QVariant& value);

    bool m_available = false;
    QVariantList m_outputs;
    QVariantList m_inputs;
    QProcess* m_subscribe = nullptr;
    QTimer m_debounce;
    bool m_loading = false;
    bool m_refreshPending = false;
    bool m_running = false;
    quint64 m_revision = 0;
    QList<QPair<QString, QStringList>> m_queue;
    // A pending mute must never be displayed as an already confirmed mute.
    QSet<QString> m_unconfirmedMutes;
    // A default-input change may have taken effect even if its command or
    // subsequent query failed. Do not target microphone mute until confirmed.
    QSet<QString> m_unconfirmedDefaults;
};
