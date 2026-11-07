// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the shell says about updates (docs/updates.md §9.2): notifications
// only, as vela-updates-client tells them. A new update once, then
// Announced, so it isn't told again, not even in the next session (§6.4);
// a click opens Settings > System > Updates. "Sign out to finish updating"
// once the running compositor was replaced, with "Sign out".

#include <QObject>
#include <QString>

#include <functional>

class NotificationServer;

namespace vela::updates {
class Updates;
}

class UpdateNotices : public QObject {
    Q_OBJECT

public:
    // `openUpdates`: Settings on the Updates page; `signOut`: ends the
    // session.
    UpdateNotices(vela::updates::Updates* updates, NotificationServer* notifications, std::function<void()> openUpdates,
        std::function<void()> signOut, QObject* parent = nullptr);

    // The notifications, 0 if none (the tests').
    uint updateNotification() const { return m_update; }
    uint reloginNotification() const { return m_relogin; }

private Q_SLOTS:
    void onChanged();
    void onAction(uint id, const QString& action);
    void onNotificationClosed(uint id, uint reason);

private:
    vela::updates::Updates* m_updates;
    NotificationServer* m_notifications;
    std::function<void()> m_openUpdates;
    std::function<void()> m_signOut;
    uint m_update = 0;
    uint m_relogin = 0;
    QString m_told; // the update last told, until the daemon hears it
    bool m_reloginTold = false; // once per session
};
