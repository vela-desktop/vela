// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "launcherentry.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusServiceWatcher>

#include <algorithm>
#include <cmath>

namespace {

QString desktopKey(QString id)
{
    id = id.toLower();
    if (id.endsWith(QLatin1String(".desktop"))) {
        id.chop(8);
    }
    return id;
}

QString shortKey(const QString& id)
{
    return id.section(u'.', -1);
}

} // namespace

LauncherEntry::LauncherEntry(QObject* parent)
    : QObject(parent)
    , m_watcher(new QDBusServiceWatcher(this))
{
    auto bus = QDBusConnection::sessionBus();
    m_watcher->setConnection(bus);
    m_watcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, &LauncherEntry::removeSender);
    bus.connect({}, {}, QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"), this,
        SLOT(update(QString, QVariantMap, QDBusMessage)));
    // Some publishers only send when a Unity-compatible shell owns this name.
    // On a shared session bus, the existing owner keeps it.
    bus.registerService(QStringLiteral("com.canonical.Unity"));
}

void LauncherEntry::update(const QString& uri, const QVariantMap& properties, const QDBusMessage& message)
{
    const QLatin1String prefix("application://");
    if (!uri.startsWith(prefix) || !message.service().startsWith(u':')) {
        return;
    }
    const QString id = desktopKey(uri.mid(prefix.size()));
    if (id.isEmpty() || id.contains(u'/') || id.contains(u'?') || id.contains(u'#')) {
        return;
    }

    const QString sender = message.service();
    const QVariant count = properties.value(QStringLiteral("count"));
    const QVariant progress = properties.value(QStringLiteral("progress"));
    const QVariant countVisible = properties.value(QStringLiteral("count-visible"));
    const QVariant progressVisible = properties.value(QStringLiteral("progress-visible"));
    const QVariant urgent = properties.value(QStringLiteral("urgent"));
    const bool validCount = count.typeId() == QMetaType::LongLong;
    const bool validProgress = progress.typeId() == QMetaType::Double && std::isfinite(progress.toDouble());
    if (!validCount && !validProgress && countVisible.typeId() != QMetaType::Bool
        && progressVisible.typeId() != QMetaType::Bool && urgent.typeId() != QMetaType::Bool) {
        return;
    }

    if (!m_watcher->watchedServices().contains(sender)) {
        m_watcher->addWatchedService(sender);
    }
    Entry& entry = m_entries[id][sender];
    if (validCount) {
        entry.count = std::max<qlonglong>(0, count.toLongLong());
    }
    if (validProgress) {
        entry.progress = std::clamp(progress.toDouble(), 0.0, 1.0);
    }
    if (countVisible.typeId() == QMetaType::Bool) {
        entry.countVisible = countVisible.toBool();
    }
    if (progressVisible.typeId() == QMetaType::Bool) {
        entry.progressVisible = progressVisible.toBool();
    }
    if (urgent.typeId() == QMetaType::Bool) {
        entry.urgent = urgent.toBool();
    }
    entry.updated = ++m_revision;
    emit changed();
}

void LauncherEntry::removeSender(const QString& sender)
{
    m_watcher->removeWatchedService(sender);
    bool removed = false;
    for (auto app = m_entries.begin(); app != m_entries.end();) {
        removed = app->remove(sender) > 0 || removed;
        if (app->isEmpty()) {
            app = m_entries.erase(app);
        } else {
            ++app;
        }
    }
    if (removed) {
        emit changed();
    }
}

LauncherEntry::Badge LauncherEntry::badge(const QString& desktopId) const
{
    const auto newest = [](const QHash<QString, Entry>& senders) {
        return std::max_element(
            senders.cbegin(), senders.cend(), [](const Entry& a, const Entry& b) { return a.updated < b.updated; });
    };
    const QString id = desktopKey(desktopId);
    if (id.isEmpty()) {
        return {};
    }
    const Entry* entry = nullptr;
    if (auto exact = m_entries.constFind(id); exact != m_entries.cend()) {
        entry = &*newest(exact.value());
    } else {
        const QString shortId = shortKey(id);
        for (auto app = m_entries.cbegin(); app != m_entries.cend(); ++app) {
            if (shortKey(app.key()) == shortId) {
                const auto candidate = newest(app.value());
                if (!entry || candidate->updated > entry->updated) {
                    entry = &*candidate;
                }
            }
        }
    }
    if (!entry) {
        return {};
    }
    return { entry->countVisible ? entry->count : 0, entry->progressVisible ? entry->progress : -1, entry->urgent };
}
