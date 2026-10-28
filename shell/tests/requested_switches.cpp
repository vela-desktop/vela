// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Switches backed by a service move at once (requested.h): Wi-Fi in quick
// settings (SystemStatus), and airplane mode. A fake NetworkManager on a
// private bus answers slowly, announces other changes on the way, refuses,
// or never does it. Bluetooth's switch is vela-bluetooth's client, tested
// in tests/bluetooth with airplane mode over both radios; here there is no
// vela-bluetooth.

#include "bluetooth/bluetooth.h"
#include "requested.h"
#include "systemstatus.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <atomic>

namespace {

const QString nm = QStringLiteral("org.freedesktop.NetworkManager");
const QString nmPath = QStringLiteral("/org/freedesktop/NetworkManager");
const QString properties = QStringLiteral("org.freedesktop.DBus.Properties");

// How the fake service answers a Set.
enum class Answer {
    Slow, // accepts, says something else changed, does it 300 ms later
    Refuse, // an error
    Never, // accepts and never does it
};

// NetworkManager's WirelessEnabled. It lives in its own thread: the clients
// read properties with blocking calls.
class FakeServices : public QDBusVirtualObject {
public:
    std::atomic<Answer> answer { Answer::Slow };
    std::atomic<bool> wireless { true };
    QString bus;

    QString introspect(const QString&) const override { return {}; }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
    {
        if (message.interface() != properties) {
            return false;
        }
        const QString name = message.arguments().value(1).toString();
        if (message.member() == QLatin1String("Get")) {
            connection.send(message.createReply(QVariant::fromValue(QDBusVariant(get(name)))));
            return true;
        }
        if (message.member() == QLatin1String("GetAll")) {
            connection.send(message.createReply(QVariant::fromValue(QVariantMap {})));
            return true;
        }
        if (message.member() != QLatin1String("Set")) {
            return false;
        }
        const bool value = message.arguments().value(2).value<QDBusVariant>().variant().toBool();
        if (answer == Answer::Refuse) {
            connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.NetworkManager.PermissionDenied"), QStringLiteral("no")));
            return true;
        }
        connection.send(message.createReply());
        if (answer == Answer::Never) {
            return true;
        }
        // On the way, another property changes: each one makes the client
        // read the state again, still the old one.
        changed(connection, nmPath, nm, { { QStringLiteral("State"), 20u } });
        QTimer::singleShot(300, this, [this, connection, value] {
            wireless = value;
            changed(connection, nmPath, nm, { { QStringLiteral("WirelessEnabled"), value } });
        });
        return true;
    }

    QVariant get(const QString& name) const
    {
        if (name == QLatin1String("WirelessEnabled")) {
            return bool(wireless);
        }
        if (name == QLatin1String("WirelessHardwareEnabled")) {
            return true;
        }
        if (name == QLatin1String("PrimaryConnection")) {
            return QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")));
        }
        return 0u;
    }

    static void changed(const QDBusConnection& connection, const QString& path, const QString& interface, const QVariantMap& values)
    {
        QDBusMessage signal = QDBusMessage::createSignal(path, properties, QStringLiteral("PropertiesChanged"));
        signal << interface << values << QStringList();
        QDBusConnection(connection).send(signal);
    }
};

} // namespace


class RequestedSwitches : public QObject {
    Q_OBJECT

    QProcess daemon;
    QThread serviceThread;
    FakeServices* fake = nullptr;
    // Its own connection, where nobody serves org.vela.Bluetooth.
    QString address;

private slots:
    void initTestCase()
    {
        daemon.start(QStringLiteral("dbus-daemon"), { QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1") });
        QVERIFY2(daemon.waitForStarted() && daemon.waitForReadyRead(), "dbus-daemon is needed");
        address = QString::fromUtf8(daemon.readAllStandardOutput().trimmed());
        qputenv("DBUS_SYSTEM_BUS_ADDRESS", address.toUtf8());

        serviceThread.start();
        fake = new FakeServices;
        fake->bus = QStringLiteral("fake-services");
        fake->moveToThread(&serviceThread);
        bool ok = false;
        QMetaObject::invokeMethod(fake, [&] {
            QDBusConnection bus = QDBusConnection::connectToBus(address, fake->bus);
            ok = bus.registerService(nm) && bus.registerVirtualObject(QStringLiteral("/"), fake, QDBusConnection::SubPath);
        }, Qt::BlockingQueuedConnection);
        QVERIFY(ok);
        QVERIFY(QDBusConnection::connectToBus(address, QStringLiteral("no-bluetooth")).isConnected());
    }

    void cleanupTestCase()
    {
        QMetaObject::invokeMethod(fake, [this] {
            QDBusConnection::disconnectFromBus(fake->bus);
            delete fake;
        }, Qt::BlockingQueuedConnection);
        serviceThread.quit();
        serviceThread.wait();
        QDBusConnection::disconnectFromBus(QStringLiteral("no-bluetooth"));
        daemon.kill();
        daemon.waitForFinished();
    }

    void init()
    {
        fake->answer = Answer::Slow;
        fake->wireless = true;
    }

    void requestedAlone()
    {
        int changes = 0;
        Requested request([&] { ++changes; }, 100);
        QCOMPARE(request.value(false), false);
        request.ask(true);
        QCOMPARE(changes, 1);
        QCOMPARE(request.value(false), true);
        request.reported(false); // the service, still on its way
        QCOMPARE(request.value(false), true);
        request.refused(0); // a request never made
        QCOMPARE(request.value(false), true);
        request.reported(true);
        QVERIFY(!request.wanted());
        QCOMPARE(changes, 1);
        // Never reported: back to the service's value when it times out.
        request.ask(true);
        QTRY_COMPARE(changes, 3);
        QCOMPARE(request.value(false), false);
        const Requested::Id off = request.ask(false);
        request.refused(off);
        QCOMPARE(changes, 5);
        QCOMPARE(request.value(true), true);
    }

    // On, off, on again: the first request's late refusal is the first's,
    // even though it asked for the same value as the last.
    void requestedOldRefusalLeavesTheLastAlone()
    {
        Requested request([] {}, 1000);
        const Requested::Id first = request.ask(true);
        request.ask(false);
        const Requested::Id last = request.ask(true);
        QVERIFY(request.pending(last));
        QVERIFY(!request.pending(first));
        request.refused(first);
        QCOMPARE(request.wanted(), std::optional<bool>(true));
        QCOMPARE(request.value(false), true);
        request.refused(last);
        QVERIFY(!request.wanted());
    }

    // Without Bluetooth, airplane mode is Wi-Fi alone.
    void airplaneModeShowsAtOnce()
    {
        vela::bluetooth::Bluetooth bluetooth(QDBusConnection(QStringLiteral("no-bluetooth")), true);
        SystemStatus status(&bluetooth);
        QVERIFY(!status.airplaneMode());
        status.setAirplaneMode(true);
        QVERIFY(status.airplaneMode());
        QVERIFY(!status.wifiEnabled());
        QTRY_VERIFY(!fake->wireless);
        QVERIFY(status.airplaneMode());
    }

    void wifiShowsAtOnceAndRefusedGoesBack()
    {
        vela::bluetooth::Bluetooth bluetooth(QDBusConnection(QStringLiteral("no-bluetooth")), true);
        SystemStatus status(&bluetooth);
        QVERIFY(status.wifiEnabled());
        status.setWifiEnabled(false);
        QVERIFY(!status.wifiEnabled());
        for (int i = 0; i < 10; ++i) {
            QTest::qWait(50);
            QVERIFY2(!status.wifiEnabled(), "the switch went back while NetworkManager worked");
        }
        QTRY_VERIFY(!fake->wireless);
        fake->answer = Answer::Refuse;
        status.setWifiEnabled(true);
        QVERIFY(status.wifiEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!status.wifiEnabled(), 1000);
    }
};

QTEST_GUILESS_MAIN(RequestedSwitches)
#include "requested_switches.moc"
