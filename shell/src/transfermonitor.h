// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QVariantList>
#include <QList>
#include <QElapsedTimer>
#include <QString>

// Aggregates transfer updates from every vela-files process over session D-Bus.
// An Explorer window owns the actual I/O; the shell only displays the jobs.
class TransferMonitor : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY changed)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY changed)

public:
    explicit TransferMonitor(QObject* parent = nullptr);
    QVariantList jobs() const;
    int activeCount() const;
    Q_INVOKABLE void cancel(const QString& owner, int id);

public Q_SLOTS:
    void onChanged(const QString& owner, int id, const QString& title,
        qlonglong done, qlonglong total, const QString& current, bool finished, const QString& error);
    void onRemoved(const QString& owner, int id);
    void onOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner);

signals:
    void changed();
    void completed(const QString& title, const QString& error);

private:
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
};
