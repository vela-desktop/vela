// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QVariantList>
#include <QList>
#include <QElapsedTimer>
#include <QString>
#include <QSet>
#include <QDBusMessage>

// Aggregates transfer updates from every vela-files process over session D-Bus.
// An Explorer window owns the actual I/O; the shell only displays the jobs.
class TransferMonitor : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY changed)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY changed)

public:
    // trustedExecutable is used by the private-bus tests. The desktop defaults
    // to the installed vela-files executable, not a service name claimed by a client.
    explicit TransferMonitor(QObject* parent = nullptr, const QString& trustedExecutable = {});
    QVariantList jobs() const;
    int activeCount() const;
    Q_INVOKABLE void cancel(const QString& owner, int id);

public Q_SLOTS:
    void onChanged(const QString& owner, int id, const QString& title,
        qlonglong done, qlonglong total, const QString& current, bool finished, const QString& error);
    void onSnapshot(const QString& owner, int id, const QString& title,
        qlonglong done, qlonglong total, const QString& current, bool finished, const QString& error);
    void onRemoved(const QString& owner, int id);
    void receiveChanged(const QString& owner, int id, const QString& title,
        qlonglong done, qlonglong total, const QString& current, bool finished,
        const QString& error, const QDBusMessage& message);
    void receiveSnapshot(const QString& owner, int id, const QString& title,
        qlonglong done, qlonglong total, const QString& current, bool finished,
        const QString& error, const QDBusMessage& message);
    void receiveRemoved(const QString& owner, int id, const QDBusMessage& message);
    void onOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner);

signals:
    void changed();
    void completed(const QString& title, const QString& error);

private:
    bool isTrustedSignal(const QString& claimedOwner, const QDBusMessage& message);
    void applyChanged(const QString& owner, int id, const QString& title,
        qlonglong done, qlonglong total, const QString& current, bool finished, const QString& error, bool notify);
    struct Job {
        QString owner;
        int id = 0;
        QString title;
        qlonglong done = 0;
        qlonglong total = 0;
        QString current;
        bool finished = false;
        QString error;
        QElapsedTimer sampleClock;
        qlonglong sampleDone = 0;
        double speedBytesPerSecond = 0;
    };
    QList<Job> m_jobs;
    QString m_trustedExecutable;
    QSet<QString> m_trustedOwners;
};
