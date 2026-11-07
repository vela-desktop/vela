// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bluetoothprompts.h"

#include "bluetooth/bluetooth.h"
#include "notifications.h"

#include <utility>

namespace {

// A code to confirm, or a request to allow, is refused; typing or showing a
// code is cancelled (docs/bluetooth.md §7.2).
QString declineResponse(const QString& kind)
{
    return kind == QLatin1String("confirm-passkey") || kind == QLatin1String("authorize-pairing")
            || kind == QLatin1String("authorize-service")
        ? QStringLiteral("reject")
        : QStringLiteral("cancel");
}

} // namespace

BluetoothPrompts::BluetoothPrompts(vela::bluetooth::Bluetooth* bluetooth, NotificationServer* notifications,
    std::function<bool()> settingsOpen, QObject* parent)
    : QObject(parent)
    , m_bluetooth(bluetooth)
    , m_notifications(notifications)
    , m_settingsOpen(std::move(settingsOpen))
{
    connect(m_bluetooth, &vela::bluetooth::Bluetooth::questionChanged, this, &BluetoothPrompts::onQuestion);
    connect(m_bluetooth, &vela::bluetooth::Bluetooth::questionClosed, this, &BluetoothPrompts::onQuestionClosed);
    connect(m_notifications, &NotificationServer::ActionInvoked, this, &BluetoothPrompts::onAction);
    connect(m_notifications, &NotificationServer::NotificationClosed, this, &BluetoothPrompts::onNotificationClosed);
    connect(m_bluetooth, &vela::bluetooth::Bluetooth::changed, this, &BluetoothPrompts::onStillVisible);
    m_bluetooth->presentQuestions();
    onQuestion();
    onStillVisible();
}

QVariantMap BluetoothPrompts::dialog() const
{
    return m_dialog ? m_bluetooth->question() : QVariantMap();
}

void BluetoothPrompts::answer(const QString& response, const QString& value)
{
    if (!m_id.isEmpty()) {
        m_bluetooth->answer(m_id, response, value);
    }
}

void BluetoothPrompts::settingsWindowsChanged()
{
    // Settings may have closed after StillVisible became true.
    onStillVisible();
}

void BluetoothPrompts::onQuestion()
{
    const QVariantMap question = m_bluetooth->question();
    const QString id = question.value(QStringLiteral("id")).toString();
    if (id != m_id) {
        // Another question took the place of the one here.
        onQuestionClosed(m_id, {});
    }
    // Settings draws its own Adds' questions.
    if (question.isEmpty() || question.value(QStringLiteral("drawnBy")).toString() != QLatin1String("shell")) {
        // Settings adopted this same question. Its notification must not
        // remain actionable: dismissing it would reject Settings' Add.
        if (id == m_id) {
            onQuestionClosed(m_id, {});
        }
        return;
    }
    const bool unsolicited = question.value(QStringLiteral("unsolicited")).toBool();
    if (id == m_id) {
        if (m_unsolicited != unsolicited) {
            // The same question changed owners/presentation after an Add.
            closeNotification();
            m_unsolicited = unsolicited;
            if (unsolicited) {
                openDialog(false);
                notify(question);
            } else {
                openDialog(true);
            }
        } else if (m_dialog) {
            // Digits typed so far on a keyboard (show-passkey).
            emit dialogChanged();
        }
        return;
    }
    m_id = id;
    m_kind = question.value(QStringLiteral("kind")).toString();
    m_unsolicited = unsolicited;
    if (unsolicited) {
        notify(question);
    } else {
        openDialog(true);
    }
}

void BluetoothPrompts::onQuestionClosed(const QString& id, const QString&)
{
    if (id.isEmpty() || id != m_id) {
        return;
    }
    closeNotification();
    m_id.clear();
    m_kind.clear();
    m_unsolicited = false;
    openDialog(false);
}

void BluetoothPrompts::notify(const QVariantMap& question)
{
    const QString name = question.value(QStringLiteral("name")).toString();
    const QString icon = question.value(QStringLiteral("icon")).toString();
    // The body is markup: a device's name is the device's to choose.
    const QString shownName = name.toHtmlEscaped();
    QString summary = tr("Pair with %1?").arg(name);
    QString body;
    QStringList actions { QStringLiteral("default"), QString() };
    if (m_kind == QLatin1String("confirm-passkey")) {
        body = tr("Check that %1 shows %2.").arg(shownName, question.value(QStringLiteral("passkey")).toString());
        actions << QStringLiteral("pair") << tr("Pair") << QStringLiteral("deny") << tr("Deny");
    } else if (m_kind == QLatin1String("authorize-pairing")) {
        body = tr("%1 wants to pair with this PC.").arg(shownName);
        actions << QStringLiteral("pair") << tr("Pair") << QStringLiteral("deny") << tr("Deny");
    } else if (m_kind == QLatin1String("authorize-service")) {
        summary = tr("Allow %1 to use %2?").arg(name, question.value(QStringLiteral("service")).toString());
        actions << QStringLiteral("allow") << tr("Allow") << QStringLiteral("deny") << tr("Deny");
    } else if (m_kind == QLatin1String("enter-passkey") || m_kind == QLatin1String("enter-pin")) {
        body = m_kind == QLatin1String("enter-passkey") ? tr("Type the 6-digit code shown on %1.").arg(shownName)
                                                        : tr("Type the PIN for %1.").arg(shownName);
        actions << QStringLiteral("enter") << tr("Enter code") << QStringLiteral("deny") << tr("Deny");
    } else {
        // show-passkey, show-pin: the code is typed on the device.
        const QString code = m_kind == QLatin1String("show-pin") ? question.value(QStringLiteral("pin")).toString()
                                                                 : question.value(QStringLiteral("passkey")).toString();
        body = tr("Type %1 on %2, then press Enter on it.").arg(code.toHtmlEscaped(), shownName);
        actions << QStringLiteral("deny") << tr("Cancel");
    }
    // Critical: it stays until answered, and passes Do not disturb.
    // Transient: an answer that's gone never waits in the notification
    // center.
    const QVariantMap hints { { QStringLiteral("urgency"), 2 }, { QStringLiteral("transient"), true } };
    m_notification = m_notifications->Notify(tr("Bluetooth"), 0,
        icon.isEmpty() || icon == QLatin1String("bluetooth") ? QStringLiteral("preferences-system-bluetooth") : icon, summary, body,
        actions, hints, 0);
}

void BluetoothPrompts::closeNotification()
{
    if (m_notification != 0) {
        const uint id = std::exchange(m_notification, 0);
        m_notifications->CloseNotification(id);
    }
}

void BluetoothPrompts::onStillVisible()
{
    if (!m_bluetooth->stillVisible()) {
        m_visibleTold = false;
        if (m_visibleNotification != 0) {
            const uint id = std::exchange(m_visibleNotification, 0);
            m_notifications->CloseNotification(id);
        }
        return;
    }
    if (m_visibleTold) {
        return;
    }
    if (m_settingsOpen && m_settingsOpen()) {
        return; // Settings says it; notify if Settings closes later.
    }
    m_visibleTold = true;
    m_visibleNotification = m_notifications->Notify(tr("Bluetooth"), 0, QStringLiteral("preferences-system-bluetooth"),
        tr("Your PC is still visible to nearby devices"), tr("Another app is searching for devices."),
        { QStringLiteral("stop"), tr("Stop being visible") }, {}, -1);
}

void BluetoothPrompts::onAction(uint id, const QString& action)
{
    if (id != 0 && id == m_visibleNotification) {
        m_visibleNotification = 0;
        if (action == QLatin1String("stop")) {
            m_bluetooth->stopBeingVisible();
        }
        return;
    }
    if (id == 0 || id != m_notification) {
        return;
    }
    // The server closes it next: that isn't a refusal.
    m_notification = 0;
    if (action == QLatin1String("pair") || action == QLatin1String("allow")) {
        m_bluetooth->answer(m_id, QStringLiteral("accept"));
    } else if (action == QLatin1String("deny")) {
        decline();
    } else {
        // "enter", or a click on the notification: the whole question.
        openDialog(true);
    }
}

void BluetoothPrompts::onNotificationClosed(uint id, uint)
{
    if (id != 0 && id == m_visibleNotification) {
        m_visibleNotification = 0; // dismissed: the PC stays visible
        return;
    }
    if (id == 0 || id != m_notification) {
        return;
    }
    m_notification = 0;
    decline();
}

void BluetoothPrompts::openDialog(bool open)
{
    if (m_dialog != open) {
        m_dialog = open;
        emit dialogChanged();
    }
}

void BluetoothPrompts::decline()
{
    m_bluetooth->answer(m_id, declineResponse(m_kind));
}
