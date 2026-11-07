// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "batterynotices.h"

#include "notifications.h"
#include "upowerbatteries.h"

#include <cmath>

namespace {

// A peripheral by its UPower Type: what to call it when it has no model, and
// its icon.
struct Kind {
    const char* name;
    const char* icon;
};

Kind kind(uint type)
{
    switch (type) {
    case 5: return { QT_TRANSLATE_NOOP("BatteryNotices", "Mouse"), "input-mouse" };
    case 6: return { QT_TRANSLATE_NOOP("BatteryNotices", "Keyboard"), "input-keyboard" };
    case 8: return { QT_TRANSLATE_NOOP("BatteryNotices", "Phone"), "phone" };
    case 10: return { QT_TRANSLATE_NOOP("BatteryNotices", "Tablet"), "input-tablet" };
    case 12: return { QT_TRANSLATE_NOOP("BatteryNotices", "Game controller"), "input-gaming" };
    case 13: return { QT_TRANSLATE_NOOP("BatteryNotices", "Pen"), "input-tablet" };
    case 14: return { QT_TRANSLATE_NOOP("BatteryNotices", "Touchpad"), "input-touchpad" };
    case 17: return { QT_TRANSLATE_NOOP("BatteryNotices", "Headset"), "audio-headset" };
    case 18: return { QT_TRANSLATE_NOOP("BatteryNotices", "Speakers"), "audio-speakers" };
    case 19: return { QT_TRANSLATE_NOOP("BatteryNotices", "Headphones"), "audio-headphones" };
    case 22: return { QT_TRANSLATE_NOOP("BatteryNotices", "Remote control"), "input-tablet" };
    default: return { QT_TRANSLATE_NOOP("BatteryNotices", "Wireless device"), "battery-caution" };
    }
}

} // namespace

BatteryNotices::BatteryNotices(NotificationServer* notifications, const QDBusConnection& bus, QObject* parent)
    : QObject(parent)
    , m_notifications(notifications)
    , m_batteries(new UPowerBatteries(bus, this))
{
    connect(m_batteries, &UPowerBatteries::changed, this,
        [this](const BatteryReading& reading) {
            carryOut(m_alerts.update(reading));
            ++m_readings;
        });
    connect(m_batteries, &UPowerBatteries::removed, this,
        [this](const QString& id) { carryOut(m_alerts.removed(id)); });
    // Dismissed: forgotten here, still told (only charging, or coming back
    // with a normal charge, rearms).
    connect(m_notifications, &NotificationServer::NotificationClosed, this, [this](uint id, uint) {
        for (auto it = m_shown.begin(); it != m_shown.end();) {
            it = it.value() == id ? m_shown.erase(it) : std::next(it);
        }
    });
}

BatteryNotices::~BatteryNotices() = default;

void BatteryNotices::start()
{
    m_batteries->start();
}

void BatteryNotices::configure(const BatteryAlerts::Settings& settings)
{
    if (!(settings == m_alerts.settings())) {
        carryOut(m_alerts.configure(settings));
    }
}

void BatteryNotices::carryOut(const std::vector<BatteryAlerts::Action>& actions)
{
    for (const BatteryAlerts::Action& action : actions) {
        if (action.kind == BatteryAlerts::Action::Kind::Close) {
            if (const uint id = m_shown.take(action.reading.id)) {
                m_notifications->CloseNotification(id);
            }
        } else {
            show(action.reading, action.level);
        }
    }
}

void BatteryNotices::show(const BatteryReading& reading, BatteryAlerts::Level level)
{
    // Critical gets a popup of its own, even if the low one is already in
    // the notification center (or Do not disturb sent it there).
    if (const uint old = m_shown.take(reading.id)) {
        m_notifications->CloseNotification(old);
    }
    const bool critical = level == BatteryAlerts::Level::Critical;
    QVariantMap hints { { QStringLiteral("urgency"), critical ? 2 : 1 } };
    QString app;
    QString icon;
    QString title;
    QString body;
    if (reading.system) {
        app = tr("Power");
        icon = critical ? QStringLiteral("battery-empty") : QStringLiteral("battery-caution");
        title = critical ? tr("Battery critically low") : tr("Battery low");
        if (reading.percentage >= 0) {
            const int left = int(std::lround(reading.percentage));
            body = critical ? tr("%1% remaining. Plug in your PC now.").arg(left)
                            : tr("%1% remaining. Plug in your PC.").arg(left);
        } else {
            body = critical ? tr("Plug in your PC now.") : tr("Plug in your PC.");
        }
    } else {
        const Kind device = kind(reading.type);
        const QString name = reading.name.isEmpty() ? tr(device.name) : reading.name;
        app = tr("Devices");
        icon = QString::fromLatin1(device.icon);
        title = critical ? tr("%1 battery is critical").arg(name) : tr("%1 battery is low").arg(name);
        body = critical ? tr("Recharge the device now.") : tr("Recharge the device soon.");
        // A mouse at its last charge needs a popup through Do not disturb,
        // not one stuck on screen until dismissed.
        if (critical) {
            hints.insert(QStringLiteral("x-vela-expires"), true);
        }
    }
    const uint id = m_notifications->Notify(app, 0, icon, title, body, {}, hints, critical && reading.system ? 0 : -1);
    m_shown.insert(reading.id, id);
}
