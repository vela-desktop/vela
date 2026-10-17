// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "transfermonitor.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QDBusPendingCall>
#include <QDBusMessage>
#include <QVariantMap>
#include <algorithm>
#include <iterator>
#include <utility>

namespace {
const QString path = QStringLiteral("/org/vela/Files/Transfers");
const QString transferInterface = QStringLiteral("org.vela.Files.Transfers");
}

TransferMonitor::TransferMonitor(QObject* parent, const QString& trustedExecutable)
    : QObject(parent)
{
    // Files and shell normally live in the same installation directory.
    // Resolve canonical inode targets: an unrelated process may claim the
    // interface and forge the payload, but not its bus-assigned PID/executable.
    QString executable = trustedExecutable;
    if (executable.isEmpty()) {
        const QString sibling = QFileInfo(QCoreApplication::applicationFilePath()).dir()
            .filePath(QStringLiteral("vela-files"));
        executable = QFileInfo::exists(sibling)
            ? sibling : QStandardPaths::findExecutable(QStringLiteral("vela-files"));
    }
    m_trustedExecutable = QFileInfo(executable).canonicalFilePath();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    // QDBusMessage is appended by Qt to signal slots. Its service() is the
    // authenticated sender set by the bus; the payload owner is untrusted.
    bus.connect(QString(), path, transferInterface, QStringLiteral("Changed"), this,
        SLOT(receiveChanged(QString,int,QString,qlonglong,qlonglong,QString,bool,QString,QDBusMessage)));
    bus.connect(QString(), path, transferInterface, QStringLiteral("Snapshot"), this,
        SLOT(receiveSnapshot(QString,int,QString,qlonglong,qlonglong,QString,bool,QString,QDBusMessage)));
    bus.connect(QString(), path, transferInterface, QStringLiteral("Removed"), this,
        SLOT(receiveRemoved(QString,int,QDBusMessage)));
    bus.connect(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameOwnerChanged"),
        this, SLOT(onOwnerChanged(QString,QString,QString)));
    // Recover active jobs if the shell was restarted while Explorer was copying.
    bus.send(QDBusMessage::createSignal(path, transferInterface, QStringLiteral("RequestSnapshot")));
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
            { QStringLiteral("speed"), job.speedBytesPerSecond },
            { QStringLiteral("eta"), job.speedBytesPerSecond > 0 && job.total > job.done
                ? double(job.total - job.done) / job.speedBytesPerSecond : -1.0 },
        });
    }
    return result;
}

int TransferMonitor::activeCount() const
{
    return int(std::count_if(m_jobs.cbegin(), m_jobs.cend(),
        [](const Job& job) { return !job.finished; }));
}

bool TransferMonitor::isTrustedSignal(const QString& claimedOwner, const QDBusMessage& message)
{
    // Reject spoofed owner names even if the attacker knows a real Explorer's
    // unique session-bus name. DBus allocates message.service() itself.
    if (!claimedOwner.startsWith(QLatin1Char(':')) || message.type() != QDBusMessage::SignalMessage
        || message.service() != claimedOwner || m_trustedExecutable.isEmpty()) {
        return false;
    }
    if (m_trustedOwners.contains(claimedOwner)) {
        return true;
    }

    // One PID lookup per Explorer connection, never once per progress sample.
    // Checking the actual executable, rather than a self-registered well-known
    // bus name, prevents a different app from impersonating Vela Files.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface()) {
        return false;
    }
    const QDBusReply<uint> pid = bus.interface()->servicePid(claimedOwner);
    if (!pid.isValid() || pid.value() == 0) {
        return false;
    }
    const QString actual = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid.value())).canonicalFilePath();
    if (actual.isEmpty() || actual != m_trustedExecutable) {
        return false;
    }
    m_trustedOwners.insert(claimedOwner);
    return true;
}

void TransferMonitor::receiveChanged(const QString& owner, int id, const QString& title,
    qlonglong done, qlonglong total, const QString& current, bool finished,
    const QString& error, const QDBusMessage& message)
{
    if (isTrustedSignal(owner, message)) {
        onChanged(owner, id, title, done, total, current, finished, error);
    }
}

void TransferMonitor::receiveSnapshot(const QString& owner, int id, const QString& title,
    qlonglong done, qlonglong total, const QString& current, bool finished,
    const QString& error, const QDBusMessage& message)
{
    if (isTrustedSignal(owner, message)) {
        onSnapshot(owner, id, title, done, total, current, finished, error);
    }
}

void TransferMonitor::receiveRemoved(const QString& owner, int id, const QDBusMessage& message)
{
    if (isTrustedSignal(owner, message)) {
        onRemoved(owner, id);
    }
}

void TransferMonitor::onChanged(const QString& owner, int id, const QString& title,
    qlonglong done, qlonglong total, const QString& current, bool finished, const QString& error)
{
    applyChanged(owner, id, title, done, total, current, finished, error, true);
}

void TransferMonitor::onSnapshot(const QString& owner, int id, const QString& title,
    qlonglong done, qlonglong total, const QString& current, bool finished, const QString& error)
{
    applyChanged(owner, id, title, done, total, current, finished, error, false);
}

void TransferMonitor::applyChanged(const QString& owner, int id, const QString& title,
    qlonglong done, qlonglong total, const QString& current, bool finished, const QString& error, bool notify)
{
    if (!owner.startsWith(QLatin1Char(':')) || id < 1) {
        return;
    }
    const auto sameJob = [&](const Job& job) { return job.owner == owner && job.id == id; };
    auto it = std::find_if(m_jobs.begin(), m_jobs.end(), sameJob);
    const bool wasFinished = it != m_jobs.end() && it->finished;
    if (it == m_jobs.end()) {
        if (m_jobs.size() >= 128) {
            auto stale = std::find_if(m_jobs.begin(), m_jobs.end(),
                [](const Job& job) { return job.finished; });
            if (stale == m_jobs.end()) return; // bound untrusted concurrent jobs
            m_jobs.erase(stale);
        }
        m_jobs.append(Job {});
        it = std::prev(m_jobs.end());
    }
    if (!it->sampleClock.isValid()) {
        it->sampleDone = done;
        it->sampleClock.start();
    } else if (!finished && done >= it->sampleDone && it->sampleClock.elapsed() >= 400) {
        const double sampled = double(done - it->sampleDone) * 1000.0 / it->sampleClock.elapsed();
        it->speedBytesPerSecond = it->speedBytesPerSecond == 0 ? sampled
            : 0.65 * it->speedBytesPerSecond + 0.35 * sampled;
        it->sampleDone = done;
        it->sampleClock.restart();
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
    if (notify && finished && !wasFinished) {
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
    Q_UNUSED(oldOwner);
    if (!name.startsWith(QLatin1Char(':')) || !newOwner.isEmpty()) {
        return;
    }
    m_trustedOwners.remove(name);
    // An unexpectedly closed Explorer process must not leave a fake 'copying'
    // indicator on the taskbar. Warn for interrupted jobs.
    for (const Job& job : std::as_const(m_jobs)) {
        if (job.owner == name && !job.finished) {
            emit completed(job.title,
                QCoreApplication::translate("Shell", "File Explorer closed before this transfer finished."));
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
    QDBusMessage msg = QDBusMessage::createMethodCall(owner, path, transferInterface, QStringLiteral("Cancel"));
    msg << id;
    QDBusConnection::sessionBus().asyncCall(msg);
}
