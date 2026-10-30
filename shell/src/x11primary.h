// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Match the X11 primary monitor and its global font DPI to Vela's selected
// main monitor. Xwayland Satellite always advertises physical output modes,
// but intentionally uses the LOWEST scale among mixed-scale outputs for its
// global XSETTINGS and RESOURCE_MANAGER DPI. Consequently, on a 1.25x primary
// plus 1.0x secondary, Steam can render at 100% even while games correctly
// discover the physical 2560x1440 mode.
//
// X11's RESOURCE_MANAGER DPI is global, not a per-output setting. We choose
// the explicitly preferred primary monitor for this legacy X11 hint, leaving
// the physical XRandR modes and Vela's Wayland output scales untouched.
#include "../../common/primaryoutput.h"

#include <QGuiApplication>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QtDebug>

#include <cmath>

class X11PrimarySync final : public QObject {
public:
    explicit X11PrimarySync(QObject* parent = nullptr) : QObject(parent)
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] { apply(); });

        // Hotplug, mode changes and fractional scale changes may update the
        // monitor identity/DPR after Xwayland Satellite has started.
        for (QScreen* screen : QGuiApplication::screens()) {
            watchScreen(screen);
        }
        connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen* screen) {
            watchScreen(screen);
            schedule();
        });
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { schedule(); });
        connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, [this] { schedule(); });
    }

    void setPreferred(const QString& name)
    {
        m_preferred = name;
        schedule();
    }

private:
    void watchScreen(QScreen* screen)
    {
        if (!screen) return;
        connect(screen, &QScreen::geometryChanged, this, [this] { schedule(); });
        connect(screen, &QScreen::physicalDotsPerInchChanged, this, [this] { schedule(); });
        connect(screen, &QScreen::logicalDotsPerInchChanged, this, [this] { schedule(); });
    }

    void schedule()
    {
        m_attempts = 0;
        m_timer.start(200); // Let Satellite process the Wayland output update.
    }

    void setPrimaryDpi(int dpi)
    {
        if (QStandardPaths::findExecutable(QStringLiteral("xrdb")).isEmpty())
            return;
        // xrdb -merge preserves all other resource keys and works with Steam's
        // Xft.dpi reader. Never set GDK_SCALE globally: that could make Proton
        // and other apps apply a second scale on top of native resolution.
        if (m_dpiProcess) {
            m_pendingDpi = dpi;
            return;
        }
        auto* process = new QProcess(this);
        m_dpiProcess = process;
        process->setProgram(QStringLiteral("xrdb"));
        process->setArguments({ QStringLiteral("-nocpp"), QStringLiteral("-merge"), QStringLiteral("-") });
        process->setProcessChannelMode(QProcess::MergedChannels);
        auto completed = [this, process](bool success) {
            if (m_dpiProcess != process) return;
            m_dpiProcess = nullptr;
            if (!success) {
                qWarning() << "vela-shell: could not publish Xft.dpi for the X11 primary monitor:"
                           << process->readAll();
            }
            process->deleteLater();
            if (m_pendingDpi > 0) {
                const int next = m_pendingDpi;
                m_pendingDpi = 0;
                setPrimaryDpi(next);
            }
        };
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [completed](int code, QProcess::ExitStatus status) {
                completed(status == QProcess::NormalExit && code == 0);
            });
        connect(process, &QProcess::errorOccurred, this,
            [completed](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) completed(false);
            });
        process->start();
        process->write(QStringLiteral("Xft.dpi:\t%1\n").arg(dpi).toUtf8());
        process->closeWriteChannel();
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

        const QScreen* selected = fallback;
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen && screen->name() == name) {
                selected = screen;
                break;
            }
        }
        // The logical UI DPI is 96 at 100%. The Wayland devicePixelRatio is
        // the relevant fractional scale, not EDID's physical inches or Qt's
        // unrelated font DPI preference.
        const int dpi = std::lround(96.0 * (selected ? selected->devicePixelRatio() : 1.0));

        auto* process = new QProcess(this);
        m_running = process;
        process->setProgram(QStringLiteral("xrandr"));
        process->setArguments({ QStringLiteral("--output"), name, QStringLiteral("--primary") });
        process->setProcessChannelMode(QProcess::MergedChannels);
        auto complete = [this, process, name, dpi](bool success) {
            if (m_running != process) return;
            m_running = nullptr;
            process->deleteLater();
            if (m_pending) {
                m_pending = false;
                schedule();
            } else if (!success && m_attempts++ < 5) {
                // Xwayland starts lazily; an output may not exist immediately.
                m_timer.start(350);
            } else if (!success) {
                qWarning() << "vela-shell: could not set XRandR primary to" << name;
            } else {
                m_attempts = 0;
                setPrimaryDpi(dpi);
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
    QPointer<QProcess> m_dpiProcess;
    bool m_pending = false;
    int m_pendingDpi = 0;
    int m_attempts = 0;
};
