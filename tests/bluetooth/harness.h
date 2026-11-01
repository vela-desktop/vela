// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the client library's and the shell's tests run on (docs/bluetooth.md
// §11): a private system bus with the fake BlueZ and logind in a thread of
// their own, a private session bus where the test's UI connection lives,
// a fake /dev/rfkill and /sys with hci0's own switch, and the real
// vela-bluetooth (its test build), started and stopped per case. A test
// class derives from it and calls setUp() and tearDown() from
// initTestCase() and cleanupTestCase().

#include "fakebluez.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

class BluetoothBench {
public:
    // `connection`: the name of the test's own connection to the session
    // bus, the UI the daemon sees.
    explicit BluetoothBench(const QString& connection)
        : m_connection(connection)
    {
    }

protected:
    QString systemAddress;
    QString sessionAddress;
    FakeBluez* fake = nullptr;
    FakeLogind* logind = nullptr;
    QTemporaryDir home;
    QProcess daemon;
    QThread fakeThread; // where the fakes live

    QDBusConnection bus() const { return QDBusConnection(m_connection); }

    // Empty, or what went wrong.
    QString setUp()
    {
        qDBusRegisterMetaType<FakeInterfaces>();
        qDBusRegisterMetaType<FakeObjects>();
        for (auto [process, address] : { std::pair { &m_systemBus, &systemAddress }, std::pair { &m_sessionBus, &sessionAddress } }) {
            process->start(QStringLiteral("dbus-daemon"), { QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1") });
            if (!process->waitForStarted() || !process->waitForReadyRead()) {
                return QStringLiteral("dbus-daemon is needed");
            }
            *address = QString::fromUtf8(process->readAllStandardOutput().trimmed());
        }
        if (!QDBusConnection::connectToBus(sessionAddress, m_connection).isConnected()) {
            return QStringLiteral("no session bus connection");
        }
        // The adapter's own rfkill switch, unblocked.
        const QString rfkillSwitch = home.filePath(QStringLiteral("sys/class/bluetooth/hci0/rfkill3"));
        if (!QDir().mkpath(rfkillSwitch) || !QDir().mkpath(home.filePath(QStringLiteral("config")))) {
            return QStringLiteral("no fake /sys");
        }
        for (const auto& [name, value] : { std::pair { "type", "bluetooth" }, std::pair { "index", "3" }, std::pair { "soft", "0" },
                 std::pair { "hard", "0" } }) {
            QFile file(rfkillSwitch + u'/' + QLatin1String(name));
            if (!file.open(QIODevice::WriteOnly)) {
                return QStringLiteral("no fake /sys");
            }
            file.write(QByteArray(value) + '\n');
        }
        QFile rfkill(home.filePath(QStringLiteral("rfkill")));
        if (!rfkill.open(QIODevice::WriteOnly)) {
            return QStringLiteral("no fake /dev/rfkill");
        }

        QSignalSpy started(&fakeThread, &QThread::started);
        fakeThread.start();
        if (started.isEmpty() && !started.wait(5000)) {
            return QStringLiteral("fake BlueZ thread did not start");
        }
        fake = new FakeBluez;
        fake->bus = QStringLiteral("fake-bluez");
        fake->moveToThread(&fakeThread);
        logind = new FakeLogind;
        logind->bus = QStringLiteral("fake-logind");
        logind->moveToThread(&fakeThread);
        bool ok = false;
        fake->run([&] {
            QDBusConnection bluez = QDBusConnection::connectToBus(systemAddress, fake->bus);
            ok = bluez.registerService(QStringLiteral("org.bluez"))
                && bluez.registerVirtualObject(QStringLiteral("/"), fake, QDBusConnection::SubPath);
            QDBusConnection login1 = QDBusConnection::connectToBus(systemAddress, logind->bus);
            ok = ok && login1.registerService(QStringLiteral("org.freedesktop.login1"))
                && login1.registerVirtualObject(QStringLiteral("/org/freedesktop/login1"), logind, QDBusConnection::SubPath);
        });
        return ok ? QString() : QStringLiteral("the fakes couldn't take their names");
    }

    void tearDown()
    {
        stopDaemon();
        if (fake && logind && fakeThread.isRunning()) {
            fake->run([this] {
                QDBusConnection::disconnectFromBus(fake->bus);
                QDBusConnection::disconnectFromBus(logind->bus);
            });
            QMetaObject::invokeMethod(fake, [this] { delete logind; delete fake; }, Qt::BlockingQueuedConnection);
        } else {
            delete logind;
            delete fake;
        }
        fake = nullptr;
        logind = nullptr;
        if (fakeThread.isRunning()) {
            fakeThread.quit();
            fakeThread.wait();
        }
        QDBusConnection::disconnectFromBus(m_connection);
        for (QProcess* process : { &m_systemBus, &m_sessionBus }) {
            process->kill();
            process->waitForFinished();
        }
    }

    // The test build of the daemon, on the private buses and the fake
    // /dev/rfkill and /sys, once it owns its name. Its start can take
    // seconds on a loaded machine.
    bool startDaemon()
    {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("DBUS_SYSTEM_BUS_ADDRESS"), systemAddress);
        environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), sessionAddress);
        environment.insert(QStringLiteral("VELA_RFKILL"), home.filePath(QStringLiteral("rfkill")));
        environment.insert(QStringLiteral("VELA_SYSFS"), home.filePath(QStringLiteral("sys")));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home.filePath(QStringLiteral("config")));
        environment.insert(QStringLiteral("XDG_SESSION_ID"), QStringLiteral("fake"));
        daemon.setProcessEnvironment(environment);
        daemon.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        daemon.start(QStringLiteral(VELA_BLUETOOTH_DAEMON), {});
        if (!daemon.waitForStarted()) {
            return false;
        }
        const QString service = QStringLiteral("org.vela.Bluetooth");
        for (int i = 0; i < 500 && !bus().interface()->isServiceRegistered(service); ++i) {
            QTest::qWait(20);
        }
        return bus().interface()->isServiceRegistered(service);
    }

    void stopDaemon()
    {
        if (daemon.state() == QProcess::NotRunning) {
            return;
        }
        daemon.terminate();
        if (!daemon.waitForFinished(5000)) {
            daemon.kill();
            daemon.waitForFinished();
        }
    }

    // Each case starts with the session in front.
    void sessionInFront()
    {
        fake->run([this] {
            logind->active = true;
            logind->locked = false;
        });
    }

private:
    QString m_connection;
    QProcess m_systemBus;
    QProcess m_sessionBus;
};
