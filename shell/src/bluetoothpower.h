// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Turning Bluetooth on and off (quick settings and Settings).
//
// BlueZ doesn't power on an adapter blocked by rfkill: "Powered" stays false
// and PowerState says "off-blocked". Plasma, for example, sets the block when
// it turns Bluetooth off. To turn it on, the soft block is removed first by
// writing to /dev/rfkill (the session user can: systemd's uaccess rule), then
// BlueZ is asked to power it on; if the unblock hasn't reached BlueZ yet, it's
// retried a moment later.

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QString>
#include <QTimer>
#include <QVariant>

#include <functional>

#include <fcntl.h>
#include <linux/rfkill.h>
#include <unistd.h>

namespace vela::bluetooth {

// Removes the soft block from all Bluetooth devices. VELA_RFKILL replaces
// /dev/rfkill in the tests, which must not unblock the machine's radios.
inline bool unblock()
{
    const QByteArray path = qEnvironmentVariableIsSet("VELA_RFKILL") ? qgetenv("VELA_RFKILL") : QByteArray("/dev/rfkill");
    const int fd = ::open(path.constData(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    rfkill_event event {};
    event.op = RFKILL_OP_CHANGE_ALL;
    event.type = RFKILL_TYPE_BLUETOOTH;
    event.soft = 0;
    const bool ok = ::write(fd, &event, RFKILL_EVENT_SIZE_V1) == RFKILL_EVENT_SIZE_V1;
    ::close(fd);
    return ok;
}

// Asks BlueZ to turn the adapter on or off. `refused()` if BlueZ says no, or
// if the adapter is still off after the retries; success shows up as BlueZ's
// own PropertiesChanged. The retries stop once `wanted()` is false (a newer
// request came, or this one timed out). Nothing blocks: callbacks
// end with `context`.
inline void setPowered(QObject* context, const QString& adapter, bool on, std::function<bool()> wanted,
    std::function<void()> refused, int attempt = 0)
{
    if (adapter.isEmpty()) {
        refused();
        return;
    }
    if (on && attempt == 0) {
        unblock();
    }
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), adapter,
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Set"));
    call << QStringLiteral("org.bluez.Adapter1") << QStringLiteral("Powered") << QVariant::fromValue(QDBusVariant(on));
    auto* reply = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call), context);
    QObject::connect(reply, &QDBusPendingCallWatcher::finished, context, [=](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        if (!on) {
            if (w->isError()) {
                refused();
            }
            return;
        }
        // BlueZ sees the unblock with some delay (and may refuse until it
        // does): until the adapter is on, it's asked again, three times.
        QTimer::singleShot(400, context, [=] {
            if (!wanted()) {
                return;
            }
            QDBusMessage get = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), adapter,
                QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
            get << QStringLiteral("org.bluez.Adapter1") << QStringLiteral("Powered");
            auto* check = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(get, 500), context);
            QObject::connect(check, &QDBusPendingCallWatcher::finished, context, [=](QDBusPendingCallWatcher* c) {
                c->deleteLater();
                const QDBusMessage message = c->reply();
                const bool powered = message.type() == QDBusMessage::ReplyMessage && !message.arguments().isEmpty()
                    && message.arguments().first().value<QDBusVariant>().variant().toBool();
                if (powered || !wanted()) {
                    return;
                }
                if (attempt < 3) {
                    setPowered(context, adapter, true, wanted, refused, attempt + 1);
                } else {
                    refused();
                }
            });
        });
    });
}

} // namespace vela::bluetooth
