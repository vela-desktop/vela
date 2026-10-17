// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "transfermonitor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QVariantMap>
#include <algorithm>

namespace {
const QString path = QStringLiteral("/org/vela/Files/Transfers");
const QString interface = QStringLiteral("org.vela.Files.Transfers");
}

TransferMonitor::TransferMonitor(QObject* parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    bus.connect(QString(), path, interface, QStringLiteral("Changed"), this,
        SLOT(onChanged(QString,int,QString,qlonglong,qlonglong,QString,bool,QString)));
    bus.connect(QString(), path, interface, QStringLiteral("Removed"), this,
        SLOT(onRemoved(QString,int)));
    bus.connect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"),
        this, SLOT(onOwnerChanged(QString,QString,QString)));
    // Recover active jobs if the shell was restarted while Explorer was copying.
    bus.send(QDBusMessage::createSignal(path, interface, QStringLiteral("RequestSnapshot")));
}

QVariantList TransferMonitor::jobs() const
{
    QVariantList result;
    for (const Job& job : m_jobs) {
        result.append(QVariantMap {
            { QStringLiteral("owner"), job.owner },
            { QStringLiteral("id"), job.id },
            { QStringLiteral("title"), job.title },
            { QStringLiteral("done"), double(job.done) },
            { QStringLiteral("total"), double(job.total) },
            { QStringLiteral("current"), job.current },
            { QStringLiteral("finished"), job.finished },
            { QStringLiteral("error"), job.error },
        });
    }
    return result;
}

int TransferMonitor::activeCount() const
{
    return int(std::count_if(m_jobs.cbegin(), m_jobs.cend(),
        [](const Job& job) { return !job.finished; }));
}

void TransferMonitor::onChanged(const QString& owner, int id, const QString& title,
    qlonglong done, qlonglong total, const QString& current, bool finished, const QString& error)
{
    if (!owner.startsWith(QLatin1Char(':')) || id < 1) {
        return;
    }
    const auto sameJob = [&](const Job& job) { return job.owner == owner && job.id == id; };
    auto it = std::find_if(m_jobs.begin(), m_jobs.end(), sameJob);
    const bool wasFinished = it != m_jobs.end() && it->finished;
    if (it == m_jobs.end()) {
        if (m_jobs.size() >= 128) {
            return; // bound untrusted session-bus updates
        }
        m_jobs.append(Job {});
        it = std::prev(m_jobs.end());
    }
    it->owner = owner;
    it->id = id;
    it->title = title.left(300);
    it->done = std::max<qlonglong>(0, done);
    it->total = std::max<qlonglong>(0, total);
    it->current = current.left(300);
    it->finished = finished;
    it->error = error.left(1000);
    emit changed();
    if (finished && !wasFinished) {
        emit completed(it->title, it->error);
    }
}

void TransferMonitor::onRemoved(const QString& owner, int id)
{
    const qsizetype before = m_jobs.size();
    m_jobs.removeIf([&](const Job& job) { return job.owner == owner && job.id == id; });
    if (m_jobs.size() != before) {
        emit changed();
    }
}

void TransferMonitor::onOwnerChanged(const QString& name, const QString& oldOwner, const QString& newOwner)
{
    Q_UNUSED(oldOwner)
    if (!name.startsWith(QLatin1Char(':')) || !newOwner.isEmpty()) {
        return;
    }
    // An unexpectedly closed Explorer process must not leave a fake 'copying'
    // indicator on the taskbar. Warn for interrupted jobs.
    for (const Job& job : std::as_const(m_jobs)) {
        if (job.owner == name && !job.finished) {
            emit completed(job.title, QStringLiteral("File Explorer closed before this transfer finished."));
        }
    }
    const qsizetype before = m_jobs.size();
    m_jobs.removeIf([&](const Job& job) { return job.owner == name; });
    if (m_jobs.size() != before) {
        emit changed();
    }
}

void TransferMonitor::cancel(const QString& owner, int id)
{
    if (std::none_of(m_jobs.cbegin(), m_jobs.cend(),
            [&](const Job& job) { return job.owner == owner && job.id == id && !job.finished; })) {
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(owner, path, interface, QStringLiteral("Cancel"));
    msg << id;
    QDBusConnection::sessionBus().asyncCall(msg);
}
