// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// Virtual desktops as the shell sees them: the compositor decides them
// (compositor/src/workspace.c) and sends the state as JSON on every change;
// commands start from here (switch to, new, close, rename, move a window...).
// Task View and the taskbar use them.
class Workspaces : public QObject {
    Q_OBJECT
    Q_PROPERTY(int current READ current NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QStringList names READ names NOTIFY changed)
    // the window's ext identifier -> desktop (-1: all)
    Q_PROPERTY(QVariantMap windows READ windows NOTIFY changed)
    Q_PROPERTY(QStringList stickyApps READ stickyApps NOTIFY changed)
    // Snap groups: [{output, windows: [{id, tile: [x0, y0, x1, y1]}]}], tile
    // in twelfths of the usable area.
    Q_PROPERTY(QVariantList snapGroups READ snapGroups NOTIFY changed)

public:
    explicit Workspaces(QObject* parent = nullptr);

    int current() const { return m_current; }
    int count() const { return int(m_names.size()); }
    QStringList names() const { return m_names; }
    QVariantMap windows() const { return m_windows; }
    QString outputOf(const QString& window) const { return m_outputs.value(window).toString(); }
    bool needsAttention(const QString& window) const { return m_attention.contains(window); }
    QStringList stickyApps() const { return m_stickyApps; }
    QVariantList snapGroups() const { return m_snapGroups; }

    // A window's desktop (-1: all; -2: unknown).
    Q_INVOKABLE int workspaceOf(const QString& window) const;

    Q_INVOKABLE void switchTo(int index);
    Q_INVOKABLE void create(bool switchTo = false);
    Q_INVOKABLE void remove(int index);
    Q_INVOKABLE void rename(int index, const QString& name);
    Q_INVOKABLE void move(int from, int to);
    Q_INVOKABLE void moveWindow(const QString& window, int index);
    Q_INVOKABLE void moveWindowToNew(const QString& window);
    Q_INVOKABLE void setWindowSticky(const QString& window, bool on);
    Q_INVOKABLE void setAppSticky(const QString& window, bool on);

    // The state sent by the compositor ("workspaces <json>").
    void update(const QByteArray& json);
    // Asks the compositor for it (when the shell starts).
    void query();

signals:
    void changed();

private:
    QStringList m_names;
    int m_current = 0;
    QVariantMap m_windows;
    QVariantMap m_outputs;
    QSet<QString> m_attention;
    QStringList m_stickyApps;
    QVariantList m_snapGroups;
};
