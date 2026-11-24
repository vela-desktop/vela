// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "trayicons.h"

#include "../../common/trayitem.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>

#include <memory>

namespace {

const QString watcherService = QStringLiteral("org.kde.StatusNotifierWatcher");
const QString watcherPath = QStringLiteral("/StatusNotifierWatcher");
const QString itemInterface = QStringLiteral("org.kde.StatusNotifierItem");
const QString propertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

} // namespace

TrayIcons::TrayIcons(QObject* parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(watcherService, watcherPath, watcherService, QStringLiteral("StatusNotifierItemRegistered"), this,
        SLOT(onRegistered(QString)));
    bus.connect(watcherService, watcherPath, watcherService, QStringLiteral("StatusNotifierItemUnregistered"), this,
        SLOT(onRegistered(QString)));
    // The shell restarting: its new watcher knows only the apps still
    // running, and none while it is away.
    auto* owner = new QDBusServiceWatcher(watcherService, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(owner, &QDBusServiceWatcher::serviceOwnerChanged, this, &TrayIcons::refresh);
    refresh();
}

void TrayIcons::onRegistered(const QString& address)
{
    Q_UNUSED(address)
    refresh();
}

void TrayIcons::refresh()
{
    // Each refresh starts over; the answers of an older one are dropped.
    const int generation = ++m_generation;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusMessage get =
        QDBusMessage::createMethodCall(watcherService, watcherPath, propertiesInterface, QStringLiteral("Get"));
    get << watcherService << QStringLiteral("RegisteredStatusNotifierItems");
    auto* call = new QDBusPendingCallWatcher(bus.asyncCall(get), this);
    connect(call, &QDBusPendingCallWatcher::finished, this, [this, generation](QDBusPendingCallWatcher* call) {
        call->deleteLater();
        const QDBusPendingReply<QDBusVariant> reply = *call;
        if (generation != m_generation) {
            return;
        }
        const QStringList addresses = reply.isError() ? QStringList() : reply.value().variant().toStringList();
        auto found = std::make_shared<QList<QVariantMap>>();
        found->resize(addresses.size());
        auto pending = std::make_shared<qsizetype>(addresses.size());
        const auto finish = [this, generation, found] {
            if (generation != m_generation) {
                return;
            }
            QVariantList items;
            QStringList keys;
            for (const QVariantMap& item : std::as_const(*found)) {
                const QString key = item.value(QStringLiteral("key")).toString();
                if (!key.isEmpty() && !keys.contains(key)) {
                    keys.append(key);
                    items.append(item);
                }
            }
            if (items != m_items) {
                m_items = items;
                emit itemsChanged();
            }
        };
        if (addresses.isEmpty()) {
            finish();
            return;
        }
        for (qsizetype i = 0; i < addresses.size(); ++i) {
            const QString& address = addresses.at(i);
            const qsizetype slash = address.indexOf(QLatin1Char('/'));
            const QString service = slash < 0 ? address : address.left(slash);
            const QString path = slash < 0 ? QStringLiteral("/StatusNotifierItem") : address.mid(slash);
            QDBusMessage all =
                QDBusMessage::createMethodCall(service, path, propertiesInterface, QStringLiteral("GetAll"));
            all << itemInterface;
            auto* item = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(all), this);
            connect(item, &QDBusPendingCallWatcher::finished, this,
                [i, found, pending, finish](QDBusPendingCallWatcher* item) {
                    item->deleteLater();
                    const QDBusPendingReply<QVariantMap> reply = *item;
                    if (!reply.isError()) {
                        const QVariantMap properties = reply.value();
                        const QString icon = properties.value(QStringLiteral("IconName")).toString();
                        (*found)[i] = QVariantMap {
                            { QStringLiteral("key"), vela::tray::key(properties) },
                            { QStringLiteral("title"), vela::tray::title(properties) },
                            { QStringLiteral("icon"),
                                icon.isEmpty() ? QStringLiteral("application-x-executable") : icon },
                        };
                    }
                    if (--*pending == 0) {
                        finish();
                    }
                });
        }
    });
}
