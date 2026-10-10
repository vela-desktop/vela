// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCoreApplication>
#include <QMap>
#include <QPointer>
#include <QProcess>
#include <QTimer>

#include <csignal>
#include <functional>
#include <memory>
#include <sys/prctl.h>

namespace vela {

struct ProcessResult {
    bool ok = false;
    QByteArray output;
    QByteArray error;
};

// Completion (including failed startup and timeout) is delivered on the
// owner's event loop. A dying owner kills the process without waiting for it:
// QProcess is deleted only after exit, since its destructor otherwise waits.
inline QProcess* runProcess(QObject* owner, const QString& program, const QStringList& arguments,
    int timeout, std::function<void(ProcessResult)> done)
{
    auto* process = new QProcess(QCoreApplication::instance());
    auto* timer = new QTimer(process);
    timer->setSingleShot(true);
    const QPointer<QObject> guard(owner);
    struct State {
        bool completed = false;
        bool timedOut = false;
    };
    auto state = std::make_shared<State>();
    auto complete = [process, timer, guard, state, done = std::move(done)](bool ok, QByteArray error = {}) {
        if (state->completed) {
            return;
        }
        state->completed = true;
        timer->stop();
        ok = ok && !state->timedOut;
        if (error.isEmpty()) {
            error = process->readAllStandardError();
        }
        // Schedule disposal before calling code which might destroy its owner.
        process->deleteLater();
        if (guard) {
            done({ ok, process->readAllStandardOutput(), error });
        }
    };
    QObject::connect(process, &QProcess::finished, process,
        [complete](int code, QProcess::ExitStatus status) { complete(code == 0 && status == QProcess::NormalExit); });
    QObject::connect(process, &QProcess::errorOccurred, process, [process, complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            // Qt can emit this synchronously from start(). Deliver completion
            // only after runProcess() has returned to its caller.
            const QByteArray errorString = process->errorString().toUtf8();
            QTimer::singleShot(0, process, [complete, errorString] {
                complete(false, errorString);
            });
        }
    });
    QObject::connect(timer, &QTimer::timeout, process, [process, state] {
        state->timedOut = true;
        process->kill();
    });
    QObject::connect(owner, &QObject::destroyed, process, [process] { process->kill(); });
    // A crashed shell/settings app must not leave service clients orphaned.
    process->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
    process->start(program, arguments);
    if (timeout > 0) {
        timer->start(timeout);
    }
    return process;
}

struct ProcessQuery {
    QString key;
    QString program;
    QStringList arguments;
    int timeout = 1000;
    bool required = true;
};

// Publish a snapshot only when every required query succeeded. Optional
// services (such as PipeWire metadata on a PulseAudio system) may be absent.
inline void queryProcesses(QObject* owner, const QList<ProcessQuery>& queries,
    std::function<void(bool, QMap<QString, QByteArray>)> done)
{
    struct Batch {
        qsizetype remaining;
        bool ok = true;
        QMap<QString, QByteArray> results;
    };
    if (queries.isEmpty()) {
        done(true, {});
        return;
    }
    auto batch = std::make_shared<Batch>(Batch { queries.size(), true, {} });
    for (const ProcessQuery& query : queries) {
        runProcess(owner, query.program, query.arguments, query.timeout,
            [batch, query, done](ProcessResult result) {
                if (result.ok) {
                    batch->results.insert(query.key, result.output);
                } else if (query.required) {
                    batch->ok = false;
                }
                if (--batch->remaining == 0) {
                    done(batch->ok, std::move(batch->results));
                }
            });
    }
}

} // namespace vela
