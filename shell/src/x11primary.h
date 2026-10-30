// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Keep the XRandR primary output in sync with Vela's per-shell preference.
// Wayland itself has no primary-output protocol: Qt's primaryScreen() does not
// update XWayland's RandR primary. In particular, Wine/Proton queries RandR
// independently of the output where Vela places the fullscreen window.
#include "../../common/primaryoutput.h"

#include <QGuiApplication>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QtDebug>

class X11PrimarySync final : public QObject {
public:
    explicit X11PrimarySync(QObject* parent = nullptr) : QObject(parent)
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] { apply(); });
        // XWayland's RandR monitor IDs can change across hotplug/mode changes.
        connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { schedule(); });
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { schedule(); });
        connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, [this] { schedule(); });
    }

    void setPreferred(const QString& name)
    {
        m_preferred = name;
        schedule();
    }

private:
    void schedule()
    {
        m_attempts = 0;
        m_timer.start(200); // Let XWayland observe the Wayland layout first.
    }

    void apply()
    {
        if (m_running) {
            m_pending = true;
            return;
        }
        if (qEnvironmentVariableIsEmpty("DISPLAY")
            || QStandardPaths::findExecutable(QStringLiteral("xrandr")).isEmpty()) {
            return;
        }
        QStringList names;
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen && !screen->name().isEmpty())
                names.append(screen->name());
        }
        const QScreen* fallback = QGuiApplication::primaryScreen();
        const QString name = vela::primaryOutputName(
            m_preferred, names, fallback ? fallback->name() : QString());
        if (name.isEmpty()) return;

        auto* process = new QProcess(this);
        m_running = process;
        process->setProgram(QStringLiteral("xrandr"));
        process->setArguments({ QStringLiteral("--output"), name, QStringLiteral("--primary") });
        process->setProcessChannelMode(QProcess::MergedChannels);
        auto complete = [this, process, name](bool success) {
            if (m_running != process) return; // Can also receive FailedToStart.
            m_running = nullptr;
            process->deleteLater();
            if (m_pending) {
                m_pending = false;
                schedule();
            } else if (!success && m_attempts++ < 5) {
                // XWayland starts lazily; an output may not exist immediately.
                m_timer.start(350);
            } else if (!success) {
                qWarning() << "vela-shell: could not set XRandR primary to" << name;
            } else {
                m_attempts = 0;
            }
        };
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [complete](int code, QProcess::ExitStatus status) {
                complete(status == QProcess::NormalExit && code == 0);
            });
        connect(process, &QProcess::errorOccurred, this,
            [complete](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) complete(false);
            });
        process->start();
    }

    QString m_preferred;
    QTimer m_timer;
    QPointer<QProcess> m_running;
    bool m_pending = false;
    int m_attempts = 0;
};
