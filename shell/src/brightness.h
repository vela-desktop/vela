// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The brightness of every output, for the shell and Settings
// (docs/brightness.md §9): only a client of the compositor, which owns it. A
// `watch brightness` connection on the command socket brings the whole state
// after every change; writes go out at once, the helper coalesces them.
//
// What a slider shows follows the rule of the service-backed switches
// (requested.h): the value asked for at once, then the compositor's once it
// has taken that request. The compositor numbers its targets (`seq`), so the
// model knows which number its request gets and an older state never moves
// the slider back. A request the compositor never takes is dropped after
// `timeoutMs`.

#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class Brightness : public QObject {
    Q_OBJECT
    // One entry per output that has brightness to show or explain:
    // name, label (as Settings names it), mechanism ("ddc", "backlight"),
    // available (a slider is shown), locked (shown, not movable: the monitor
    // isn't responding), pending, refused (the monitor kept its value),
    // reason (the compositor's state: "ready", "no-answer"... §7.1) and
    // value (0-1).
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY changed)
    // A view showing a slider: the outputs are read live (§5.5).
    Q_PROPERTY(bool live READ live WRITE setLive NOTIFY liveChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)

public:
    // `socketPath`: the compositor's command socket; empty for the session's.
    explicit Brightness(QObject* parent = nullptr, const QString& socketPath = {}, int timeoutMs = 3000);
    ~Brightness() override;

    QVariantList outputs() const;
    bool live() const { return m_live; }
    void setLive(bool on);
    bool connected() const { return m_connected; }

    // The entry for an output, or an empty map.
    Q_INVOKABLE QVariantMap output(const QString& name) const;
    Q_INVOKABLE void setValue(const QString& name, double value);
    // The wheel: one notch, like the volume's keys.
    Q_INVOKABLE void step(const QString& name, int direction);
    // Settings is showing this output: probe it again (§8).
    Q_INVOKABLE void probe(const QString& name);

    static constexpr double wheelStep = 0.02;

signals:
    void changed();
    void liveChanged();

private:
    struct Output {
        QString name, label, mechanism, state;
        double value = 0; // the compositor's, -1 before the monitor answered
        bool pending = false, refused = false;
        quint32 max = 0;
        quint64 seq = 0;
    };
    // A request not yet taken: shown instead of the compositor's value.
    struct Wanted {
        double value;
        quint32 raw;
        quint64 expect; // the compositor's seq once it has taken it
        qint64 deadline;
    };

    void connectToCompositor();
    void read();
    void apply(const QByteArray& line);
    void send(const QByteArray& line);
    void expire();
    static bool available(const Output& output);

    QString m_path;
    int m_timeout;
    QLocalSocket m_socket;
    QTimer m_retry;
    QTimer m_expiry;
    QByteArray m_buffer;
    bool m_connected = false;
    bool m_live = false;
    QList<Output> m_outputs;
    QHash<QString, Wanted> m_wanted;
};
