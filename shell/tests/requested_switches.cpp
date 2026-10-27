// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Switches backed by a service move at once (requested.h): Bluetooth's power
// in quick settings and Settings' home (SystemStatus) and on Settings'
// Bluetooth page (Bluetooth), and Wi-Fi. A fake BlueZ and NetworkManager on a
// private bus answer slowly, announce other changes on the way, refuse, or
// never do it. The real radios are never touched (VELA_RFKILL, VELA_SYSFS):
// turning Bluetooth on unblocks only the adapter's own rfkill switch.

#include "bluetooth.h"
#include "bluetoothpower.h"
#include "requested.h"
#include "systemstatus.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstring>

#include <linux/rfkill.h>

namespace {

using Interfaces = QMap<QString, QVariantMap>;
using Objects = QMap<QDBusObjectPath, Interfaces>;

const QString adapterPath = QStringLiteral("/org/bluez/hci0");
const QString adapterInterface = QStringLiteral("org.bluez.Adapter1");
const QString nm = QStringLiteral("org.freedesktop.NetworkManager");
const QString nmPath = QStringLiteral("/org/freedesktop/NetworkManager");
const QString properties = QStringLiteral("org.freedesktop.DBus.Properties");

// How the fake service answers a Set.
enum class Answer {
    Slow, // accepts, says something else changed, does it 300 ms later
    Refuse, // an error
    Never, // accepts and never does it
};

// BlueZ's adapter and NetworkManager's WirelessEnabled. It lives in its own
// thread: the clients read properties with blocking calls.
class FakeServices : public QDBusVirtualObject {
public:
    std::atomic<Answer> answer { Answer::Slow };
    std::atomic<bool> powered { false };
    std::atomic<bool> wireless { true };
    std::atomic<int> powerSets { 0 };
    std::atomic<bool> lastPowerSet { false };
    QString bus;

    QString introspect(const QString&) const override { return {}; }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
    {
        const QString path = message.path();
        if (message.interface() == QLatin1String("org.freedesktop.DBus.ObjectManager")) {
            Objects objects;
            objects[QDBusObjectPath(adapterPath)][adapterInterface]
                = QVariantMap { { QStringLiteral("Powered"), bool(powered) }, { QStringLiteral("Alias"), QStringLiteral("fake") } };
            connection.send(message.createReply(QVariant::fromValue(objects)));
            return true;
        }
        if (message.interface() != properties) {
            return false;
        }
        const QString name = message.arguments().value(1).toString();
        if (message.member() == QLatin1String("Get")) {
            connection.send(message.createReply(QVariant::fromValue(QDBusVariant(get(path, name)))));
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
        const bool adapter = path == adapterPath;
        if (adapter) {
            ++powerSets;
            lastPowerSet = value;
        }
        if (answer == Answer::Refuse) {
            connection.send(message.createErrorReply(QStringLiteral("org.bluez.Error.Failed"), QStringLiteral("no")));
            return true;
        }
        connection.send(message.createReply());
        if (answer == Answer::Never) {
            return true;
        }
        // On the way, another property changes: each one makes the client
        // read the state again, still the old one.
        if (adapter) {
            changed(connection, adapterPath, adapterInterface, { { QStringLiteral("PowerState"), QStringLiteral("off-enabling") } });
        } else {
            changed(connection, nmPath, nm, { { QStringLiteral("State"), 20u } });
        }
        QTimer::singleShot(300, this, [this, connection, adapter, value] {
            if (adapter) {
                powered = value;
                changed(connection, adapterPath, adapterInterface, { { QStringLiteral("Powered"), value } });
            } else {
                wireless = value;
                changed(connection, nmPath, nm, { { QStringLiteral("WirelessEnabled"), value } });
            }
        });
        return true;
    }

    QVariant get(const QString& path, const QString& name) const
    {
        if (path == adapterPath && name == QLatin1String("Powered")) {
            return bool(powered);
        }
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

    // From the test's thread: something outside Vela turns the adapter off
    // (a hardware key).
    void powerOffOutside()
    {
        QMetaObject::invokeMethod(this, [this] {
            powered = false;
            changed(QDBusConnection(bus), adapterPath, adapterInterface, { { QStringLiteral("Powered"), false } });
        });
    }
};

// A sysfs file with one value.
bool writeValue(const QString& path, const QByteArray& value)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(value + '\n') == value.size() + 1;
}

} // namespace

Q_DECLARE_METATYPE(Objects)

class RequestedSwitches : public QObject {
    Q_OBJECT

    QProcess daemon;
    QThread serviceThread;
    FakeServices* fake = nullptr;
    QTemporaryDir home;

private slots:
    // The rfkill switch of adapter `hci`, under the fake /sys.
    QString rfkillSwitch(const QString& hci) const
    {
        const QDir device(home.filePath(QStringLiteral("sys/class/bluetooth/") + hci));
        return device.filePath(device.entryList({ QStringLiteral("rfkill*") }, QDir::Dirs).value(0));
    }

    // What was written to the fake /dev/rfkill since the last call.
    QByteArray takeRfkillWrites()
    {
        QFile rfkill(home.filePath(QStringLiteral("rfkill")));
        if (!rfkill.open(QIODevice::ReadWrite)) {
            return {};
        }
        const QByteArray written = rfkill.readAll();
        rfkill.resize(0);
        return written;
    }

    void initTestCase()
    {
        qputenv("VELA_RFKILL", home.filePath(QStringLiteral("rfkill")).toUtf8());
        QFile rfkill(home.filePath(QStringLiteral("rfkill")));
        QVERIFY(rfkill.open(QIODevice::WriteOnly));
        // Two adapters, both soft-blocked: hci0 (BlueZ's adapter here) with
        // switch 3, a USB dongle hci1 with switch 7.
        qputenv("VELA_SYSFS", home.filePath(QStringLiteral("sys")).toUtf8());
        for (const auto& [hci, index] : { std::pair { "hci0", "3" }, std::pair { "hci1", "7" } }) {
            const QString path = home.filePath(QStringLiteral("sys/class/bluetooth/%1/rfkill%2").arg(QLatin1String(hci), QLatin1String(index)));
            QVERIFY(QDir().mkpath(path));
            QVERIFY(writeValue(path + QStringLiteral("/index"), index));
        }
        qDBusRegisterMetaType<Interfaces>();
        qDBusRegisterMetaType<Objects>();

        daemon.start(QStringLiteral("dbus-daemon"), { QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1") });
        QVERIFY2(daemon.waitForStarted() && daemon.waitForReadyRead(), "dbus-daemon is needed");
        const QString address = QString::fromUtf8(daemon.readAllStandardOutput().trimmed());
        qputenv("DBUS_SYSTEM_BUS_ADDRESS", address.toUtf8());

        serviceThread.start();
        fake = new FakeServices;
        fake->bus = QStringLiteral("fake-services");
        fake->moveToThread(&serviceThread);
        bool ok = false;
        QMetaObject::invokeMethod(fake, [&] {
            QDBusConnection bus = QDBusConnection::connectToBus(address, fake->bus);
            ok = bus.registerService(QStringLiteral("org.bluez")) && bus.registerService(nm)
                && bus.registerVirtualObject(QStringLiteral("/"), fake, QDBusConnection::SubPath);
        }, Qt::BlockingQueuedConnection);
        QVERIFY(ok);
    }

    void cleanupTestCase()
    {
        QMetaObject::invokeMethod(fake, [this] {
            QDBusConnection::disconnectFromBus(fake->bus);
            delete fake;
        }, Qt::BlockingQueuedConnection);
        serviceThread.quit();
        serviceThread.wait();
        daemon.kill();
        daemon.waitForFinished();
    }

    void init()
    {
        fake->answer = Answer::Slow;
        fake->powered = false;
        fake->wireless = true;
        fake->powerSets = 0;
        for (const QString& hci : { QStringLiteral("hci0"), QStringLiteral("hci1") }) {
            QVERIFY(writeValue(rfkillSwitch(hci) + QStringLiteral("/soft"), "1"));
            QVERIFY(writeValue(rfkillSwitch(hci) + QStringLiteral("/hard"), "0"));
        }
        takeRfkillWrites();
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

    void bluetoothShowsAtOnceAndStays()
    {
        SystemStatus status;
        QVERIFY(status.bluetoothAvailable());
        QVERIFY(!status.bluetoothEnabled());
        QSignalSpy changed(&status, &SystemStatus::bluetoothChanged);
        status.setBluetoothEnabled(true);
        QVERIFY(status.bluetoothEnabled());
        QCOMPARE(changed.count(), 1);
        // The PowerState change and the delay: it never goes back.
        for (int i = 0; i < 10; ++i) {
            QTest::qWait(50);
            QVERIFY2(status.bluetoothEnabled(), "the switch went back while BlueZ worked");
        }
        QTRY_VERIFY(fake->powered);
        QVERIFY(status.bluetoothEnabled());
        // Done: what BlueZ says is shown again.
        fake->powerOffOutside();
        QTRY_VERIFY_WITH_TIMEOUT(!status.bluetoothEnabled(), 1000);
    }

    // Turning on removes the soft block of hci0's own switch, once, with
    // RFKILL_OP_CHANGE: never CHANGE_ALL, which would also unblock hci1 and
    // any platform Bluetooth switch.
    void bluetoothUnblocksOnlyTheAdaptersOwnSwitch()
    {
        SystemStatus status;
        status.setBluetoothEnabled(true);
        QTRY_VERIFY(fake->powered);
        const QByteArray written = takeRfkillWrites();
        QCOMPARE(written.size(), qsizetype(RFKILL_EVENT_SIZE_V1));
        rfkill_event event {};
        memcpy(&event, written.constData(), RFKILL_EVENT_SIZE_V1);
        QCOMPARE(int(event.op), int(RFKILL_OP_CHANGE));
        QCOMPARE(event.idx, 3u);
        QCOMPARE(int(event.type), int(RFKILL_TYPE_BLUETOOTH));
        QCOMPARE(int(event.soft), 0);
        // Turning off never blocks.
        status.setBluetoothEnabled(false);
        QTRY_VERIFY(!fake->powered);
        QVERIFY(takeRfkillWrites().isEmpty());
    }

    // Nothing to write when the switch isn't soft-blocked, or is
    // hard-blocked; nor for a path that isn't an adapter.
    void bluetoothUnblockLeavesOtherSwitchesAlone()
    {
        QVERIFY(writeValue(rfkillSwitch(QStringLiteral("hci0")) + QStringLiteral("/hard"), "1"));
        QVERIFY(!vela::bluetooth::unblock(adapterPath));
        QVERIFY(writeValue(rfkillSwitch(QStringLiteral("hci0")) + QStringLiteral("/hard"), "0"));
        QVERIFY(writeValue(rfkillSwitch(QStringLiteral("hci0")) + QStringLiteral("/soft"), "0"));
        QVERIFY(!vela::bluetooth::unblock(adapterPath));
        QVERIFY(!vela::bluetooth::unblock(QStringLiteral("/org/bluez/hci0/../hci1")));
        QVERIFY(!vela::bluetooth::unblock(QStringLiteral("/org/bluez/hci2")));
        QVERIFY(takeRfkillWrites().isEmpty());
        // The other adapter is reached only through its own path.
        QVERIFY(vela::bluetooth::unblock(QStringLiteral("/org/bluez/hci1")));
        const QByteArray written = takeRfkillWrites();
        QCOMPARE(written.size(), qsizetype(RFKILL_EVENT_SIZE_V1));
        rfkill_event event {};
        memcpy(&event, written.constData(), RFKILL_EVENT_SIZE_V1);
        QCOMPARE(event.idx, 7u);
    }

    void bluetoothRefusedGoesBack()
    {
        fake->powered = true;
        SystemStatus status;
        QVERIFY(status.bluetoothEnabled());
        fake->answer = Answer::Refuse;
        status.setBluetoothEnabled(false);
        QVERIFY(!status.bluetoothEnabled());
        // From BlueZ's answer, well before the request would time out.
        QTRY_VERIFY_WITH_TIMEOUT(status.bluetoothEnabled(), 1000);
        QVERIFY(fake->powered);
    }

    void bluetoothThatNeverPowersGoesBack()
    {
        SystemStatus status;
        fake->answer = Answer::Never;
        status.setBluetoothEnabled(true);
        QVERIFY(status.bluetoothEnabled());
        // Three retries, 400 ms apart, then back off.
        QTRY_VERIFY_WITH_TIMEOUT(!status.bluetoothEnabled(), 4000);
        QCOMPARE(int(fake->powerSets), 4);
    }

    void bluetoothRetriesStopWhenTheUserChangesTheirMind()
    {
        SystemStatus status;
        fake->answer = Answer::Never;
        status.setBluetoothEnabled(true);
        status.setBluetoothEnabled(false);
        QVERIFY(!status.bluetoothEnabled());
        QTest::qWait(1500);
        QCOMPARE(int(fake->powerSets), 2);
        QCOMPARE(bool(fake->lastPowerSet), false);
    }

    void airplaneModeShowsAtOnce()
    {
        fake->powered = true;
        SystemStatus status;
        QVERIFY(!status.airplaneMode());
        status.setAirplaneMode(true);
        QVERIFY(status.airplaneMode());
        QVERIFY(!status.wifiEnabled());
        QVERIFY(!status.bluetoothEnabled());
        QTRY_VERIFY(!fake->powered && !fake->wireless);
        QVERIFY(status.airplaneMode());
    }

    void wifiShowsAtOnceAndRefusedGoesBack()
    {
        SystemStatus status;
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

    void settingsBluetoothPageShowsAtOnce()
    {
        Bluetooth bluetooth;
        bluetooth.refresh(); // as the page does when it opens
        QVERIFY(bluetooth.available());
        QVERIFY(!bluetooth.powered());
        bluetooth.setPowered(true);
        QVERIFY(bluetooth.powered());
        for (int i = 0; i < 10; ++i) {
            QTest::qWait(50);
            QVERIFY2(bluetooth.powered(), "the switch went back while BlueZ worked");
        }
        QTRY_VERIFY(fake->powered);
        QVERIFY(bluetooth.powered());
        fake->answer = Answer::Refuse;
        bluetooth.setPowered(false);
        QVERIFY(!bluetooth.powered());
        QTRY_VERIFY_WITH_TIMEOUT(bluetooth.powered(), 1000);
    }
};

QTEST_GUILESS_MAIN(RequestedSwitches)
#include "requested_switches.moc"
