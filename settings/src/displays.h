// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

struct wl_display;

// Native output-management client. Vela owns the preview snapshot and deadline,
// restoring it when this client's connection disappears, including on a crash.
class Displays : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY outputsChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool pending READ pending NOTIFY stateChanged)
    Q_PROPERTY(bool confirming READ confirming NOTIFY stateChanged)
    Q_PROPERTY(int secondsRemaining READ secondsRemaining NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
    explicit Displays(QObject* parent = nullptr);
    // Takes ownership of a dedicated connection (also used by protocol tests).
    Displays(wl_display* connection, QObject* parent);
    ~Displays() override;

    bool available() const;
    bool pending() const;
    bool confirming() const;
    int secondsRemaining() const;
    QString error() const;
    QVariantList outputs() const;

    Q_INVOKABLE void refresh();
    // Returns whether the asynchronous request was submitted.
    Q_INVOKABLE bool apply(const QString& name, const QVariantMap& changes);
    Q_INVOKABLE void confirm();
    Q_INVOKABLE void revert();

signals:
    void outputsChanged();
    void stateChanged();
    void confirmationRequested();
    void confirmationFinished();

private:
    struct Client;
    std::unique_ptr<Client> m_client;
};
