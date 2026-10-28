// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Bluetooth as the shell and Settings show it (docs/bluetooth.md §8): a
// client of vela-bluetooth's org.vela.Bluetooth, fed by DevicesChanged and
// the daemon's properties, as lists for QML. Without the daemon everything
// is empty and `available` false; when it comes back the model is read
// again. The switch shows what the user asked for until BlueZ does it or
// refuses (requested.h). Vela's own UI also receives the agent's
// questions (§7.2).

#include "requested.h"

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QMap>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class QDBusMessage;

namespace vela::bluetooth {

class Bluetooth : public QObject {
    Q_OBJECT
    // The daemon runs, and BlueZ has an adapter.
    Q_PROPERTY(bool available READ available NOTIFY changed)
    // No adapter, and a platform Bluetooth switch is blocked (§6.3): the
    // switch shows Off and turning it on unblocks it.
    Q_PROPERTY(bool platformBlocked READ platformBlocked NOTIFY changed)
    // The session is in front (§6.7): what changes something works.
    Q_PROPERTY(bool active READ active NOTIFY changed)
    // The switch: the adapter's power, or what was asked for it.
    Q_PROPERTY(bool powered READ powered NOTIFY changed)
    // The adapter in use: {path, address, name, powered, powerState,
    // discoverable, discovering}; empty without one.
    Q_PROPERTY(QVariantMap adapter READ adapter NOTIFY changed)
    // [{path, address, name, inUse}]
    Q_PROPERTY(QVariantList adapters READ adapters NOTIFY changed)
    // The adapter's devices: [{path, address, name, icon, type, paired,
    //   trusted, connected, blocked, battery, rssi, operation, error,
    //   message, waiting}]; battery: -1 when unknown; rssi: only while
    //   searching, 0 otherwise; operation: the running step ("pairing",
    //   "connecting"...), or empty; error, message: the last operation's
    //   failure; waiting: a question about it is open.
    // paired: the paired ones, connected first, then by name. found: the
    // others, nearest first.
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    Q_PROPERTY(QVariantList paired READ paired NOTIFY changed)
    Q_PROPERTY(QVariantList found READ found NOTIFY changed)
    // The open question, for Vela's UI (§7.2): {id, device, kind, name,
    //   icon, address, unsolicited, drawnBy, deadline, passkey, entered,
    //   pin, uuid, service}; empty when none. passkey: six digits; deadline:
    //   when BlueZ gives up, in Date.now()'s milliseconds.
    Q_PROPERTY(QVariantMap question READ question NOTIFY questionChanged)
    // The device being added: by this client, or by anyone, or asking to
    // pair by itself; no other Add can start meanwhile (§6.5).
    Q_PROPERTY(QString pairing READ pairing NOTIFY changed)
    // A search lease is held (§6.6). searchEnded: the last one ended, and
    // a new one has to be asked for ("Search again").
    Q_PROPERTY(bool searching READ searching NOTIFY searchChanged)
    Q_PROPERTY(bool searchEnded READ searchEnded NOTIFY searchChanged)
    // Vela searches without making the PC visible, because another app was
    // already searching (§6.6, rule 1).
    Q_PROPERTY(bool searchShared READ searchShared NOTIFY changed)
    // The PC is still visible after Vela's search, because another app is
    // searching (§6.6, rule 3): "Stop being visible".
    Q_PROPERTY(bool stillVisible READ stillVisible NOTIFY changed)
    // The last refusal that isn't a device's own failure, in plain words;
    // empty if none.
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

public:
    // `ui`: Vela's own UI (vela-shell, vela-settings), which receives the
    // questions and draws an Add's questions itself (§7.3).
    explicit Bluetooth(bool ui, QObject* parent = nullptr);
    Bluetooth(const QDBusConnection& bus, bool ui, QObject* parent = nullptr);

    bool available() const { return m_running && m_available; }
    bool platformBlocked() const { return m_running && !m_available && m_platformBlocked; }
    bool active() const { return m_active; }
    bool powered() const;
    QVariantMap adapter() const { return m_adapter; }
    QVariantList adapters() const { return m_adapters; }
    QVariantList devices() const;
    QVariantList paired() const;
    QVariantList found() const;
    QVariantMap question() const { return m_question; }
    QString pairing() const;
    bool searching() const { return m_lease != 0; }
    bool searchEnded() const { return m_searchEnded; }
    bool searchShared() const { return m_search == QLatin1String("shared"); }
    bool stillVisible() const { return m_stillVisible; }
    QString error() const { return m_error; }

    Q_INVOKABLE void setPowered(bool on);
    Q_INVOKABLE void useAdapter(const QString& address);
    // Pairs, trusts and connects: "Add a device". added(path) once done.
    Q_INVOKABLE void add(const QString& path);
    Q_INVOKABLE void connectDevice(const QString& path);
    Q_INVOKABLE void disconnectDevice(const QString& path);
    Q_INVOKABLE void removeDevice(const QString& path);
    Q_INVOKABLE void rename(const QString& path, const QString& name);
    Q_INVOKABLE void cancel(const QString& path);
    // response: "accept", "reject" or "cancel"; value: the passkey or PIN
    // typed (§7.2).
    Q_INVOKABLE void answer(const QString& id, const QString& response, const QString& value = {});
    // A discoverable search for "Add a device" (§6.6), until stopSearch()
    // or the lease ends.
    Q_INVOKABLE void startSearch();
    Q_INVOKABLE void stopSearch();
    // "Your PC is still visible": the user's explicit action (§6.6, rule 3).
    Q_INVOKABLE void stopBeingVisible();
    // This process draws the questions no Add of a UI is behind (the
    // shell, §9.3): until one does, the daemon refuses them at once. Asked
    // again whenever the daemon restarts.
    void presentQuestions();
    Q_INVOKABLE void clearError();

signals:
    void changed();
    void questionChanged();
    void searchChanged();
    void errorChanged();
    // An Add of this client ended with the device paired.
    void added(const QString& path);
    // The question closed: "answered", "cancelled-by-device"... (§7.2).
    void questionClosed(const QString& id, const QString& reason);

private Q_SLOTS:
    void onOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner);
    void onPropertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList& invalidated);
    void onDevicesChanged(const QDBusMessage& message);
    void onQuestion(const QDBusMessage& message);
    void onQuestionChanged(const QDBusMessage& message);
    void onQuestionClosed(qulonglong id, const QString& reason);
    void onSearchEnded(qulonglong lease, const QString& reason);

private:
    // Where a refusal goes: error(); error() but a Failed, which the
    // device's own entry shows; the log, for what no user asked for.
    enum class Refusal { Shown, OnDevice, Logged };

    QDBusMessage method(const QString& name, const QVariantList& arguments = {}) const;
    // Calls `name`; `done` gets the reply, after the refusal (if any) is
    // shown.
    void call(const QString& name, const QVariantList& arguments, int timeout,
        const std::function<void(const QDBusMessage&)>& done = {}, Refusal refusal = Refusal::Shown);
    void deviceCall(const QString& name, const QString& path, const QVariantList& extra = {});
    void appeared();
    void vanished();
    void setProperties(const QVariantMap& values);
    void setDevice(const QString& path, const QVariantMap& details);
    void setQuestion(qulonglong id, const QString& device, const QVariantMap& details);
    void dropQuestion();
    void refused(const QDBusMessage& reply, bool onDevice);
    void setError(const QString& error);
    void endSearch(bool ended);

    QDBusConnection m_bus;
    bool m_ui;
    bool m_presenting = false;
    int m_presentRetries = 0;
    quint64 m_ownerGeneration = 0; // Ignore replies from a previous daemon owner.
    bool m_running = false;
    bool m_available = false;
    bool m_platformBlocked = false;
    bool m_active = true;
    QVariantMap m_adapter;
    QVariantList m_adapters;
    QMap<QString, QVariantMap> m_devices; // by path, as the daemon gives them
    QVariantMap m_question;
    QString m_adding; // the device of this client's Add
    quint64 m_lease = 0;
    bool m_searchWanted = false; // startSearch() and no stopSearch() since
    bool m_searchStarting = false; // StartSearch hasn't replied yet
    bool m_searchEnded = false;
    QString m_search; // the daemon's Search: "", "visible", "shared", "plain"
    bool m_stillVisible = false;
    QString m_error;
    Requested m_power;
};

} // namespace vela::bluetooth
