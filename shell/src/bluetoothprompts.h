// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The Bluetooth agent's questions the shell draws (docs/bluetooth.md §9.3):
// those whose `drawnBy` is "shell". One asked for by the user (an Add whose
// Settings has gone, or one without a UI of its own) opens the question
// dialog at once. One a device asks by itself (a phone pairing, a service)
// is a notification first: Pair or Allow, and Deny; a code to type opens the
// dialog ("Enter code"), as a click on the notification does. A
// notification closed without an answer refuses the question. Everything
// closes when the question does, wherever it was answered.

#include <QObject>
#include <QString>
#include <QVariantMap>

class NotificationServer;

namespace vela::bluetooth {
class Bluetooth;
}

class BluetoothPrompts : public QObject {
    Q_OBJECT
    // The question the dialog shows, as Vela.Bluetooth gives it; empty when
    // the dialog is closed.
    Q_PROPERTY(QVariantMap dialog READ dialog NOTIFY dialogChanged)

public:
    BluetoothPrompts(vela::bluetooth::Bluetooth* bluetooth, NotificationServer* notifications, QObject* parent = nullptr);

    QVariantMap dialog() const;
    // From the dialog: "accept" with the code typed, if any; "reject" or
    // "cancel" (§7.2).
    Q_INVOKABLE void answer(const QString& response, const QString& value = {});

    // The notification of the open question, 0 if none (the tests').
    uint notification() const { return m_notification; }

signals:
    void dialogChanged();

private Q_SLOTS:
    void onQuestion();
    void onQuestionClosed(const QString& id, const QString& reason);
    void onAction(uint id, const QString& action);
    void onNotificationClosed(uint id, uint reason);

private:
    void notify(const QVariantMap& question);
    void closeNotification();
    void openDialog(bool open);
    void decline();

    vela::bluetooth::Bluetooth* m_bluetooth;
    NotificationServer* m_notifications;
    QString m_id; // the question handled here, empty if none
    QString m_kind;
    uint m_notification = 0;
    bool m_dialog = false;
};
