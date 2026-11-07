// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "updatenotices.h"

#include "notifications.h"
#include "updates/updates.h"

#include <utility>

UpdateNotices::UpdateNotices(vela::updates::Updates* updates, NotificationServer* notifications,
    std::function<void()> openUpdates, std::function<void()> signOut, QObject* parent)
    : QObject(parent)
    , m_updates(updates)
    , m_notifications(notifications)
    , m_openUpdates(std::move(openUpdates))
    , m_signOut(std::move(signOut))
{
    connect(m_updates, &vela::updates::Updates::changed, this, &UpdateNotices::onChanged);
    connect(m_notifications, &NotificationServer::ActionInvoked, this, &UpdateNotices::onAction);
    connect(m_notifications, &NotificationServer::NotificationClosed, this, &UpdateNotices::onNotificationClosed);
    onChanged();
}

void UpdateNotices::onChanged()
{
    const QString commit = m_updates->announce();
    if (!commit.isEmpty() && commit != m_told) {
        m_told = commit;
        // One at a time: a newer update replaces the older one's.
        // The body is markup; nothing in it comes from outside.
        m_update = m_notifications->Notify(tr("Updates"), m_update, QStringLiteral("system-software-update"),
            tr("Vela %1 is available").arg(m_updates->versionText()),
            tr("Open Updates to see what's new and install it."),
            { QStringLiteral("default"), QString(), QStringLiteral("open"), tr("Open Updates") }, {}, -1);
        // Told: the daemon won't ask again, here or in another session.
        m_updates->announced(commit);
    }
    if (m_updates->needsRelogin() && !m_reloginTold) {
        m_reloginTold = true;
        // It stays until the user does something about it.
        m_relogin = m_notifications->Notify(tr("Updates"), 0, QStringLiteral("system-log-out"),
            tr("Sign out to finish updating"),
            tr("Vela was updated. Sign out and sign in again to use the new version."),
            { QStringLiteral("default"), QString(), QStringLiteral("sign-out"), tr("Sign out") }, {}, 0);
    }
}

void UpdateNotices::onAction(uint id, const QString& action)
{
    if (id == 0) {
        return;
    }
    if (id == m_update) {
        m_update = 0;
        m_openUpdates();
    } else if (id == m_relogin) {
        m_relogin = 0;
        if (action == QLatin1String("sign-out")) {
            m_signOut();
        } else {
            m_openUpdates();
        }
    }
}

void UpdateNotices::onNotificationClosed(uint id, uint)
{
    if (id != 0 && id == m_update) {
        m_update = 0;
    } else if (id != 0 && id == m_relogin) {
        m_relogin = 0;
    }
}
