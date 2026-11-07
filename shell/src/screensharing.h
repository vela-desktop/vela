// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Who is sharing the screen or a window (docs/screen-sharing.md §5, §6): a
// client of the compositor's `watch capture`, which knows; this only names
// the apps and what they capture. The compositor may start after us, or
// restart: the connection is tried again every second.

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <functional>

class ScreenSharing : public QObject {
    Q_OBJECT
    // An app outside the shell is capturing; true for about a second more
    // after the last one stops, so that the icon doesn't blink.
    Q_PROPERTY(bool active READ active NOTIFY changed)
    // One entry per app, in the order the compositor lists them: name, icon,
    // desktopId (may be empty), window (an identifier to bring forward, may be
    // empty) and what (what it captures, one line: "Your screen", "A window of
    // Konsole, Dell U2720Q").
    Q_PROPERTY(QVariantList users READ users NOTIFY changed)
    // For the tooltip: "Firefox is sharing your screen"...
    Q_PROPERTY(QString summary READ summary NOTIFY changed)

public:
    struct App {
        QString desktopId, name, icon;
    };
    // Names an app from its ids (an app id, a binary...), as Start does.
    using Resolver = std::function<App(const QStringList& candidates)>;

    // `socketPath`: the compositor's command socket; empty for the session's.
    explicit ScreenSharing(Resolver resolver, QObject* parent = nullptr, const QString& socketPath = {},
        int graceMs = 1000);
    ~ScreenSharing() override;

    bool active() const { return !m_users.isEmpty(); }
    QVariantList users() const { return m_users; }
    QString summary() const { return summarize(m_users); }

    // The apps receiving the portal's streams (Audio::screenUsers, §5.2):
    // name, icon and desktopId of each.
    void setPortalApps(const QVariantList& apps);

    // A line of `watch capture`.
    void apply(const QByteArray& line);

    // The language changed (or Start's names): everything named again.
    void refresh();

    // The compositor's state, named (§5.1). `outputs`: how many the session
    // has (one: "Your screen").
    static QVariantList describe(const QJsonObject& state, const Resolver& resolver, const QVariantList& portalApps,
        int outputs);
    static QString summarize(const QVariantList& users);

signals:
    void changed();

private:
    void connectToCompositor();
    void read();
    void update();

    Resolver m_resolver;
    QString m_path;
    QLocalSocket m_socket;
    QTimer m_retry;
    QTimer m_grace;
    QByteArray m_buffer;
    QJsonObject m_state;
    QVariantList m_portalApps;
    QVariantList m_users;
};
