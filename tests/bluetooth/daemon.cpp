// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-bluetooth against a fake BlueZ (docs/bluetooth.md §11): the model,
// the adapter in use, each operation and the ways it ends early, power and
// rfkill, and who may call. The real daemon binary runs on two private
// buses, a fake /dev/rfkill and a fake /sys; this test is the UI the test
// build trusts.

#include "fakebluez.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <linux/rfkill.h>

#include <cstring>

namespace {

using DeviceMap = QMap<QDBusObjectPath, QVariantMap>;

const QString service = QStringLiteral("org.vela.Bluetooth");
const QString objectPath = QStringLiteral("/org/vela/Bluetooth");
const QString hci0 = QStringLiteral("hci0");
const QString hci1 = QStringLiteral("hci1");
const QString mouseAddress = QStringLiteral("00:11:22:33:44:01");
const QString headsetAddress = QStringLiteral("00:11:22:33:44:02");
const QString keyboardAddress = QStringLiteral("00:11:22:33:44:03");

bool writeValue(const QString& path, const QByteArray& value)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(value + '\n') == value.size() + 1;
}

// One OperationChanged.
struct Operation {
    QString device;
    QString state;
    QString error;
    QString message;
};

} // namespace

Q_DECLARE_METATYPE(DeviceMap)

class BluetoothDaemon : public QObject {
    Q_OBJECT

    QProcess systemBus;
    QProcess sessionBus;
    QString systemAddress;
    QString sessionAddress;
    QThread fakeThread;
    FakeBluez* fake = nullptr;
    QTemporaryDir home;
    QProcess daemon;
    QList<Operation> operations;
    QString mouse;
    QString headset;
    QString keyboard;

    QDBusConnection client() const { return QDBusConnection(QStringLiteral("client")); }

    QDBusMessage method(const QString& name, const QVariantList& arguments = {}) const
    {
        QDBusMessage call = QDBusMessage::createMethodCall(service, objectPath, service, name);
        call.setArguments(arguments);
        return call;
    }

    QDBusMessage call(const QString& name, const QVariantList& arguments = {}) const
    {
        return client().call(method(name, arguments), QDBus::Block, 5000);
    }

    QDBusPendingCall start(const QString& name, const QVariantList& arguments = {}) const
    {
        return client().asyncCall(method(name, arguments), 120000);
    }

    static QVariant object(const QString& path) { return QVariant::fromValue(QDBusObjectPath(path)); }

    QVariant property(const QString& name) const
    {
        QDBusMessage get = QDBusMessage::createMethodCall(service, objectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("Get"));
        get << service << name;
        const QDBusMessage reply = client().call(get, QDBus::Block, 5000);
        return reply.arguments().value(0).value<QDBusVariant>().variant();
    }

    QVariantMap adapterInUse() const { return qdbus_cast<QVariantMap>(property(QStringLiteral("Adapter"))); }

    QMap<QString, QVariantMap> devices() const
    {
        const QDBusMessage reply = call(QStringLiteral("Devices"));
        QMap<QString, QVariantMap> devices;
        if (reply.type() != QDBusMessage::ReplyMessage) {
            return devices;
        }
        const DeviceMap map = qdbus_cast<DeviceMap>(reply.arguments().value(0));
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            devices[it.key().path()] = it.value();
        }
        return devices;
    }

    // Waits for a pending call's reply; the error name, or empty.
    static QString errorOf(QDBusPendingCall pending, int timeout = 10000)
    {
        QDBusPendingCallWatcher watcher(pending);
        if (!pending.isFinished()) {
            QSignalSpy finished(&watcher, &QDBusPendingCallWatcher::finished);
            finished.wait(timeout);
        }
        if (!pending.isFinished()) {
            return QStringLiteral("(no reply)");
        }
        return pending.isError() ? pending.error().name() : QString();
    }

    static QString errorOf(const QDBusMessage& reply)
    {
        return reply.type() == QDBusMessage::ErrorMessage ? reply.errorName() : QString();
    }

    QStringList states(const QString& device) const
    {
        QStringList states;
        for (const Operation& operation : operations) {
            if (operation.device == device) {
                states << operation.state;
            }
        }
        return states;
    }

    QStringList fakeCalls()
    {
        QStringList calls;
        fake->run([&] { calls = fake->calls; });
        return calls;
    }

    QStringList fakeWrites()
    {
        QStringList writes;
        fake->run([&] { writes = fake->writes; });
        return writes;
    }

    QString rfkillSwitch(const QString& dir) const { return home.filePath(QStringLiteral("sys/") + dir); }

    // What was written to the fake /dev/rfkill since the last call: the
    // switch numbers.
    QList<int> takeUnblocked()
    {
        QFile rfkill(home.filePath(QStringLiteral("rfkill")));
        QList<int> indexes;
        if (!rfkill.open(QIODevice::ReadWrite)) {
            return indexes;
        }
        const QByteArray written = rfkill.readAll();
        rfkill.resize(0);
        for (qsizetype at = 0; at + qsizetype(RFKILL_EVENT_SIZE_V1) <= written.size(); at += RFKILL_EVENT_SIZE_V1) {
            rfkill_event event {};
            memcpy(&event, written.constData() + at, RFKILL_EVENT_SIZE_V1);
            if (event.op == RFKILL_OP_CHANGE && event.soft == 0) {
                indexes << int(event.idx);
            } else {
                indexes << -1;
            }
        }
        return indexes;
    }

    bool startDaemon()
    {
        operations.clear();
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("DBUS_SYSTEM_BUS_ADDRESS"), systemAddress);
        environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), sessionAddress);
        environment.insert(QStringLiteral("VELA_RFKILL"), home.filePath(QStringLiteral("rfkill")));
        environment.insert(QStringLiteral("VELA_SYSFS"), home.filePath(QStringLiteral("sys")));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home.filePath(QStringLiteral("config")));
        daemon.setProcessEnvironment(environment);
        daemon.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        daemon.start(QStringLiteral(VELA_BLUETOOTH_DAEMON), {});
        if (!daemon.waitForStarted()) {
            return false;
        }
        QDBusConnection bus = client();
        for (int i = 0; i < 100 && !bus.interface()->isServiceRegistered(service); ++i) {
            QTest::qWait(20);
        }
        return bus.interface()->isServiceRegistered(service);
    }

    int stopDaemon()
    {
        if (daemon.state() == QProcess::NotRunning) {
            return daemon.exitCode();
        }
        daemon.terminate();
        if (!daemon.waitForFinished(5000)) {
            daemon.kill();
            daemon.waitForFinished();
            return -1;
        }
        return daemon.exitStatus() == QProcess::NormalExit ? daemon.exitCode() : -1;
    }

    // The fake's devices and adapters as each test starts: hci0 on, a mouse
    // found nearby, a headset paired and connected, a keyboard.
    void resetFake()
    {
        fake->run([this] {
            fake->calls.clear();
            fake->writes.clear();
            for (const QString& path : fake->devices.keys()) {
                fake->removeDevice(path);
            }
            for (const QString& name : fake->adapters.keys()) {
                fake->removeAdapter(name);
            }
            fake->addAdapter(hci0, { QStringLiteral("AA:AA:AA:AA:AA:00"), QStringLiteral("vela-pc") });
            FakeBluez::Device mouseDevice;
            mouseDevice.adapter = hci0;
            mouseDevice.address = mouseAddress;
            mouseDevice.alias = QStringLiteral("Mouse");
            mouseDevice.icon = QStringLiteral("input-mouse");
            mouseDevice.rssi = -50;
            mouse = fake->addDevice(mouseDevice);
            FakeBluez::Device headsetDevice;
            headsetDevice.adapter = hci0;
            headsetDevice.address = headsetAddress;
            headsetDevice.alias = QStringLiteral("Headset");
            headsetDevice.icon = QStringLiteral("audio-headset");
            headsetDevice.paired = headsetDevice.trusted = headsetDevice.connected = true;
            headsetDevice.battery = 80;
            headset = fake->addDevice(headsetDevice);
            FakeBluez::Device keyboardDevice;
            keyboardDevice.adapter = hci0;
            keyboardDevice.address = keyboardAddress;
            keyboardDevice.alias = QStringLiteral("Keyboard");
            keyboardDevice.icon = QStringLiteral("input-keyboard");
            keyboard = fake->addDevice(keyboardDevice);
            fake->calls.clear();
            fake->writes.clear();
        });
    }

    template<typename Change>
    void onDevice(const QString& path, Change change)
    {
        fake->run([&] { change(fake->devices[path]); });
    }

private slots:
    void onOperation(const QDBusObjectPath& device, const QString& state, const QString& error, const QString& message)
    {
        operations << Operation { device.path(), state, error, message };
    }

    void initTestCase()
    {
        qDBusRegisterMetaType<FakeInterfaces>();
        qDBusRegisterMetaType<FakeObjects>();
        qDBusRegisterMetaType<DeviceMap>();
        for (auto [bus, address] : { std::pair { &systemBus, &systemAddress }, std::pair { &sessionBus, &sessionAddress } }) {
            bus->start(QStringLiteral("dbus-daemon"), { QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1") });
            QVERIFY2(bus->waitForStarted() && bus->waitForReadyRead(), "dbus-daemon is needed");
            *address = QString::fromUtf8(bus->readAllStandardOutput().trimmed());
        }
        QDBusConnection bus = QDBusConnection::connectToBus(sessionAddress, QStringLiteral("client"));
        QVERIFY(bus.isConnected());
        QVERIFY(bus.connect(service, objectPath, service, QStringLiteral("OperationChanged"), this,
            SLOT(onOperation(QDBusObjectPath, QString, QString, QString))));

        // Two adapters with their switches, a Wi-Fi switch, and a laptop's
        // platform Bluetooth switch (asus-wmi): 3, 7, 1 and 5.
        for (const auto& [dir, type, index] : { std::tuple { "class/bluetooth/hci0/rfkill3", "bluetooth", "3" },
                 std::tuple { "class/bluetooth/hci1/rfkill7", "bluetooth", "7" },
                 std::tuple { "class/rfkill/rfkill3", "bluetooth", "3" }, std::tuple { "class/rfkill/rfkill7", "bluetooth", "7" },
                 std::tuple { "class/rfkill/rfkill1", "wlan", "1" }, std::tuple { "class/rfkill/rfkill5", "bluetooth", "5" } }) {
            const QString path = rfkillSwitch(QLatin1String(dir));
            QVERIFY(QDir().mkpath(path));
            QVERIFY(writeValue(path + QStringLiteral("/type"), type));
            QVERIFY(writeValue(path + QStringLiteral("/index"), index));
        }
        QFile rfkill(home.filePath(QStringLiteral("rfkill")));
        QVERIFY(rfkill.open(QIODevice::WriteOnly));
        QVERIFY(QDir().mkpath(home.filePath(QStringLiteral("config"))));

        fakeThread.start();
        fake = new FakeBluez;
        fake->bus = QStringLiteral("fake-bluez");
        fake->moveToThread(&fakeThread);
        bool ok = false;
        fake->run([&] {
            QDBusConnection bluez = QDBusConnection::connectToBus(systemAddress, fake->bus);
            ok = bluez.registerService(QStringLiteral("org.bluez"))
                && bluez.registerVirtualObject(QStringLiteral("/"), fake, QDBusConnection::SubPath);
        });
        QVERIFY(ok);
    }

    void cleanupTestCase()
    {
        fake->run([this] { QDBusConnection::disconnectFromBus(fake->bus); });
        QMetaObject::invokeMethod(fake, [this] { delete fake; }, Qt::BlockingQueuedConnection);
        fakeThread.quit();
        fakeThread.wait();
        QDBusConnection::disconnectFromBus(QStringLiteral("client"));
        for (QProcess* bus : { &systemBus, &sessionBus }) {
            bus->kill();
            bus->waitForFinished();
        }
    }

    void init()
    {
        fake->run([this] { fake->comeBack(); });
        resetFake();
        for (const QString& dir : { QStringLiteral("class/bluetooth/hci0/rfkill3"), QStringLiteral("class/bluetooth/hci1/rfkill7"),
                 QStringLiteral("class/rfkill/rfkill3"), QStringLiteral("class/rfkill/rfkill7"), QStringLiteral("class/rfkill/rfkill1"),
                 QStringLiteral("class/rfkill/rfkill5") }) {
            QVERIFY(writeValue(rfkillSwitch(dir) + QStringLiteral("/soft"), "0"));
            QVERIFY(writeValue(rfkillSwitch(dir) + QStringLiteral("/hard"), "0"));
        }
        QFile::remove(home.filePath(QStringLiteral("config/vela/vela.conf")));
        takeUnblocked();
        QVERIFY(startDaemon());
        QTRY_VERIFY(property(QStringLiteral("Available")).toBool());
        QTRY_COMPARE(devices().size(), 3);
    }

    void cleanup()
    {
        // Nothing left open, whatever the test did.
        if (daemon.state() == QProcess::Running) {
            QTRY_VERIFY_WITH_TIMEOUT(
                [this] {
                    for (const QVariantMap& device : devices()) {
                        if (device.contains(QStringLiteral("operation"))) {
                            return false;
                        }
                    }
                    return true;
                }(),
                5000);
        }
        // Vela never changes adapter-wide settings on its own (§6.7).
        for (const QString& write : fakeWrites()) {
            for (const char* name : { " Discoverable=", " DiscoverableTimeout=", " Pairable=", " PairableTimeout=" }) {
                QVERIFY2(!write.contains(QLatin1String(name)), qPrintable(write));
            }
            QVERIFY2(!(write.startsWith(QLatin1String("/org/bluez/hci")) && write.count(u'/') == 3 && write.contains(QLatin1String(" Alias="))),
                qPrintable(write));
        }
        QCOMPARE(stopDaemon(), 0);
    }

    // ---------------------------------------------------------- the model --

    void modelHasTheAdaptersDevices()
    {
        const QMap<QString, QVariantMap> list = devices();
        const QVariantMap mouseEntry = list.value(mouse);
        QCOMPARE(mouseEntry.value(QStringLiteral("name")).toString(), QStringLiteral("Mouse"));
        QCOMPARE(mouseEntry.value(QStringLiteral("type")).toString(), QStringLiteral("mouse"));
        QCOMPARE(mouseEntry.value(QStringLiteral("address")).toString(), mouseAddress);
        QCOMPARE(mouseEntry.value(QStringLiteral("paired")).toBool(), false);
        QCOMPARE(mouseEntry.value(QStringLiteral("rssi")).toInt(), -50);
        QVERIFY(!mouseEntry.contains(QStringLiteral("battery")));
        const QVariantMap headsetEntry = list.value(headset);
        QCOMPARE(headsetEntry.value(QStringLiteral("type")).toString(), QStringLiteral("headset"));
        QCOMPARE(headsetEntry.value(QStringLiteral("connected")).toBool(), true);
        QCOMPARE(headsetEntry.value(QStringLiteral("battery")).toInt(), 80);
        QCOMPARE(adapterInUse().value(QStringLiteral("address")).toString(), QStringLiteral("AA:AA:AA:AA:AA:00"));
        QCOMPARE(adapterInUse().value(QStringLiteral("powered")).toBool(), true);
        // Changes come as DevicesChanged.
        QSignalSpy changed(this, &BluetoothDaemon::devicesChanged);
        QVERIFY(client().connect(service, objectPath, service, QStringLiteral("DevicesChanged"), this, SIGNAL(devicesChanged())));
        fake->run([this] { fake->setDevice(headset, QStringLiteral("Connected"), false); });
        QTRY_VERIFY(changed.count() > 0);
        QCOMPARE(devices().value(headset).value(QStringLiteral("connected")).toBool(), false);
        client().disconnect(service, objectPath, service, QStringLiteral("DevicesChanged"), this, SIGNAL(devicesChanged()));
    }

    // bluetoothd restarts: everything goes, then comes back.
    void modelFollowsBluezRestarting()
    {
        fake->run([this] { QVERIFY(fake->leave()); });
        QTRY_VERIFY(!property(QStringLiteral("Available")).toBool());
        QCOMPARE(devices().size(), 0);
        QVERIFY(adapterInUse().isEmpty());
        fake->run([this] { QVERIFY(fake->comeBack()); });
        QTRY_VERIFY(property(QStringLiteral("Available")).toBool());
        QTRY_COMPARE(devices().size(), 3);
    }

    void startsWithoutBluez()
    {
        QCOMPARE(stopDaemon(), 0);
        fake->run([this] { QVERIFY(fake->leave()); });
        QVERIFY(startDaemon());
        QTest::qWait(200);
        QVERIFY(!property(QStringLiteral("Available")).toBool());
        fake->run([this] { QVERIFY(fake->comeBack()); });
        QTRY_VERIFY(property(QStringLiteral("Available")).toBool());
        QTRY_COMPARE(devices().size(), 3);
    }

    void secondInstanceExits()
    {
        QProcess second;
        second.setProcessEnvironment(daemon.processEnvironment());
        second.start(QStringLiteral(VELA_BLUETOOTH_DAEMON), {});
        QVERIFY(second.waitForFinished(5000));
        QCOMPARE(second.exitCode(), 2);
    }

    // ------------------------------------------------- the adapter in use --

    void adapterFallsBackAndComesBack()
    {
        fake->run([this] { fake->addAdapter(hci1, { QStringLiteral("BB:BB:BB:BB:BB:01"), QStringLiteral("dongle") }); });
        QTRY_COMPARE(qdbus_cast<QList<QVariantMap>>(property(QStringLiteral("Adapters"))).size(), 2);
        QCOMPARE(adapterInUse().value(QStringLiteral("address")).toString(), QStringLiteral("AA:AA:AA:AA:AA:00"));
        // hci0 goes: hci1 is used, with its own (no) devices.
        fake->run([this] { fake->removeAdapter(hci0); });
        QTRY_COMPARE(adapterInUse().value(QStringLiteral("address")).toString(), QStringLiteral("BB:BB:BB:BB:BB:01"));
        QCOMPARE(devices().size(), 0);
        // hci0 comes back: the first adapter is used again.
        fake->run([this] { fake->addAdapter(hci0, { QStringLiteral("AA:AA:AA:AA:AA:00"), QStringLiteral("vela-pc") }); });
        QTRY_COMPARE(adapterInUse().value(QStringLiteral("address")).toString(), QStringLiteral("AA:AA:AA:AA:AA:00"));
    }

    void chosenAdapterIsRemembered()
    {
        fake->run([this] { fake->addAdapter(hci1, { QStringLiteral("BB:BB:BB:BB:BB:01"), QStringLiteral("dongle") }); });
        QTRY_COMPARE(qdbus_cast<QList<QVariantMap>>(property(QStringLiteral("Adapters"))).size(), 2);
        QCOMPARE(errorOf(call(QStringLiteral("UseAdapter"), { QStringLiteral("CC:CC:CC:CC:CC:CC") })),
            QStringLiteral("org.vela.Bluetooth.Error.UnknownAdapter"));
        QCOMPARE(errorOf(call(QStringLiteral("UseAdapter"), { QStringLiteral("BB:BB:BB:BB:BB:01") })), QString());
        QCOMPARE(adapterInUse().value(QStringLiteral("address")).toString(), QStringLiteral("BB:BB:BB:BB:BB:01"));
        QFile config(home.filePath(QStringLiteral("config/vela/vela.conf")));
        QVERIFY(config.open(QIODevice::ReadOnly));
        QVERIFY(config.readAll().contains("bluetooth-adapter=BB:BB:BB:BB:BB:01"));
        // Still chosen after a restart, and when the dongle comes back.
        QCOMPARE(stopDaemon(), 0);
        QVERIFY(startDaemon());
        QTRY_COMPARE(adapterInUse().value(QStringLiteral("address")).toString(), QStringLiteral("BB:BB:BB:BB:BB:01"));
        fake->run([this] { fake->removeAdapter(hci1); });
        QTRY_COMPARE(adapterInUse().value(QStringLiteral("address")).toString(), QStringLiteral("AA:AA:AA:AA:AA:00"));
        fake->run([this] { fake->addAdapter(hci1, { QStringLiteral("BB:BB:BB:BB:BB:01"), QStringLiteral("dongle") }); });
        QTRY_COMPARE(adapterInUse().value(QStringLiteral("address")).toString(), QStringLiteral("BB:BB:BB:BB:BB:01"));
    }

    // -------------------------------------------------------- operations --

    void addPairsTrustsAndConnects()
    {
        QCOMPARE(errorOf(start(QStringLiteral("Add"), { object(mouse), false })), QString());
        const QVariantMap entry = devices().value(mouse);
        QVERIFY(entry.value(QStringLiteral("paired")).toBool());
        QVERIFY(entry.value(QStringLiteral("trusted")).toBool());
        QVERIFY(entry.value(QStringLiteral("connected")).toBool());
        QVERIFY(!entry.contains(QStringLiteral("operation")));
        QVERIFY(!entry.contains(QStringLiteral("error")));
        QTRY_COMPARE(states(mouse),
            (QStringList { QStringLiteral("pairing"), QStringLiteral("trusting"), QStringLiteral("connecting"), QStringLiteral("done") }));
        QVERIFY(fakeWrites().contains(mouse + QStringLiteral(" Trusted=true")));
    }

    void addOnAPairedDeviceConnects()
    {
        onDevice(keyboard, [](FakeBluez::Device& device) { device.paired = true; device.trusted = true; });
        QCOMPARE(errorOf(start(QStringLiteral("Add"), { object(keyboard), false })), QString());
        QVERIFY(devices().value(keyboard).value(QStringLiteral("connected")).toBool());
    }

    void addGoesOnWhenTrustFails()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.trustError = QStringLiteral("org.bluez.Error.Failed"); });
        QCOMPARE(errorOf(start(QStringLiteral("Add"), { object(mouse), false })), QString());
        const QVariantMap entry = devices().value(mouse);
        QVERIFY(entry.value(QStringLiteral("connected")).toBool());
        QCOMPARE(entry.value(QStringLiteral("error")).toString(), QStringLiteral("not-trusted"));
        QTRY_COMPARE(operations.last().state, QStringLiteral("done"));
        QCOMPARE(operations.last().error, QStringLiteral("not-trusted"));
    }

    void addThatCannotConnectStaysPaired()
    {
        onDevice(mouse, [](FakeBluez::Device& device) {
            device.connectError = QStringLiteral("org.bluez.Error.Failed");
            device.connectMessage = QStringLiteral("br-connection-page-timeout");
        });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(add.error().message(), QStringLiteral("Paired, but couldn't connect."));
        const QVariantMap entry = devices().value(mouse);
        QVERIFY(entry.value(QStringLiteral("paired")).toBool());
        QVERIFY(!entry.value(QStringLiteral("connected")).toBool());
        QCOMPARE(entry.value(QStringLiteral("error")).toString(), QStringLiteral("paired-not-connected"));
    }

    void addRefusedOnTheDevice()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.pairError = QStringLiteral("org.bluez.Error.AuthenticationRejected"); });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(add.error().message(), QStringLiteral("Pairing was refused on the device."));
        QTRY_COMPARE(operations.last().state, QStringLiteral("failed"));
        QCOMPARE(operations.last().error, QStringLiteral("rejected"));
        QCOMPARE(devices().value(mouse).value(QStringLiteral("error")).toString(), QStringLiteral("rejected"));
    }

    void connectAndDisconnect()
    {
        QCOMPARE(errorOf(start(QStringLiteral("Disconnect"), { object(headset) })), QString());
        QVERIFY(!devices().value(headset).value(QStringLiteral("connected")).toBool());
        // Not connected any more: still done.
        QCOMPARE(errorOf(start(QStringLiteral("Disconnect"), { object(headset) })), QString());
        QCOMPARE(errorOf(start(QStringLiteral("Connect"), { object(headset) })), QString());
        QVERIFY(devices().value(headset).value(QStringLiteral("connected")).toBool());
        QCOMPARE(errorOf(start(QStringLiteral("Connect"), { object(headset) })), QString());
        onDevice(headset, [](FakeBluez::Device& device) { device.connectError = QStringLiteral("org.bluez.Error.Failed"); });
        QCOMPARE(errorOf(start(QStringLiteral("Disconnect"), { object(headset) })), QString());
        QCOMPARE(errorOf(start(QStringLiteral("Connect"), { object(headset) })), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(errorOf(start(QStringLiteral("Connect"), { object(QStringLiteral("/org/bluez/hci0/dev_FF")) })),
            QStringLiteral("org.vela.Bluetooth.Error.UnknownDevice"));
    }

    void renameAndRemove()
    {
        QCOMPARE(errorOf(start(QStringLiteral("Rename"), { object(headset), QStringLiteral("Cuffie") })), QString());
        QCOMPARE(devices().value(headset).value(QStringLiteral("name")).toString(), QStringLiteral("Cuffie"));
        QCOMPARE(errorOf(start(QStringLiteral("Rename"), { object(headset), QStringLiteral("bad\nname") })),
            QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"));
        QCOMPARE(errorOf(start(QStringLiteral("Remove"), { object(headset) })), QString());
        QVERIFY(!devices().contains(headset));
        QVERIFY(fakeCalls().contains(QStringLiteral("RemoveDevice ") + headset));
    }

    // ------------------------------------------------- ending early (§6.3) --

    void addEndsWhenTheDeviceGoes()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.pairHangs = true; });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("Pair ") + mouse));
        QCOMPARE(devices().value(mouse).value(QStringLiteral("operation")).toString(), QStringLiteral("pairing"));
        fake->run([this] { fake->removeDevice(mouse); });
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(add.error().message(), QStringLiteral("The device is no longer available."));
    }

    void addEndsWhenTheAdapterTurnsOff()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.pairHangs = true; });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("Pair ") + mouse));
        fake->run([this] { fake->setPowered(hci0, false); });
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QTRY_COMPARE(operations.last().error, QStringLiteral("adapter-off"));
        // While off, an Add fails at once.
        QCOMPARE(errorOf(start(QStringLiteral("Add"), { object(keyboard), false })), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QVERIFY(!fakeCalls().contains(QStringLiteral("Pair ") + keyboard));
    }

    void addEndsWhenBluezLeaves()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.pairHangs = true; });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("Pair ") + mouse));
        fake->run([this] { fake->leave(); });
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(add.error().message(), QStringLiteral("Bluetooth isn't running."));
    }

    void cancelStopsPairing()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.pairHangs = true; });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("Pair ") + mouse));
        QCOMPARE(errorOf(call(QStringLiteral("Cancel"), { object(mouse) })), QString());
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Cancelled"));
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("CancelPairing ") + mouse));
        QTRY_COMPARE(operations.last().state, QStringLiteral("cancelled"));
        // The user's own Cancel gives no message.
        QVERIFY(operations.last().error.isEmpty());
        QVERIFY(!devices().value(mouse).contains(QStringLiteral("error")));
    }

    void cancelStopsConnecting()
    {
        onDevice(headset, [](FakeBluez::Device& device) { device.connectDelay = 2000; });
        QCOMPARE(errorOf(start(QStringLiteral("Disconnect"), { object(headset) })), QString());
        QDBusPendingCall connect = start(QStringLiteral("Connect"), { object(headset) });
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("Connect ") + headset));
        QCOMPARE(errorOf(call(QStringLiteral("Cancel"), { object(headset) })), QString());
        QCOMPARE(errorOf(connect), QStringLiteral("org.vela.Bluetooth.Error.Cancelled"));
        QTRY_COMPARE(fakeCalls().count(QStringLiteral("Disconnect ") + headset), 2);
    }

    // -------------------------------------------- one Add at a time (§6.5) --

    void oneAddAtATime()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.pairHangs = true; });
        QDBusPendingCall first = start(QStringLiteral("Add"), { object(mouse), false });
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("Pair ") + mouse));
        // Another device: refused at once, naming the one pairing.
        QDBusPendingCall other = start(QStringLiteral("Add"), { object(keyboard), false });
        QCOMPARE(errorOf(other), QStringLiteral("org.vela.Bluetooth.Error.Busy"));
        QCOMPARE(other.error().message(), mouse);
        // The same device joins.
        QDBusPendingCall second = start(QStringLiteral("Add"), { object(mouse), false });
        // Anything else on it waits.
        QCOMPARE(errorOf(start(QStringLiteral("Connect"), { object(mouse) })), QStringLiteral("org.vela.Bluetooth.Error.Busy"));
        // Other devices go on.
        QCOMPARE(errorOf(start(QStringLiteral("Disconnect"), { object(headset) })), QString());
        QCOMPARE(fakeCalls().count(QStringLiteral("Pair ") + mouse), 1);
        QCOMPARE(errorOf(call(QStringLiteral("Cancel"), { object(mouse) })), QString());
        QCOMPARE(errorOf(first), QStringLiteral("org.vela.Bluetooth.Error.Cancelled"));
        QCOMPARE(errorOf(second), QStringLiteral("org.vela.Bluetooth.Error.Cancelled"));
        // Free again.
        QCOMPARE(errorOf(start(QStringLiteral("Add"), { object(keyboard), false })), QString());
    }

    // ------------------------------------------------- power and rfkill --

    void powerOffAndOnUnblocksOnlyTheAdaptersSwitch()
    {
        QCOMPARE(errorOf(start(QStringLiteral("SetPowered"), { false })), QString());
        QVERIFY(!adapterInUse().value(QStringLiteral("powered")).toBool());
        // Turning off never blocks.
        QCOMPARE(takeUnblocked(), QList<int>());
        // Blocked by another desktop: hci0's switch, the dongle's and the
        // platform's.
        for (const char* dir : { "class/bluetooth/hci0/rfkill3", "class/bluetooth/hci1/rfkill7", "class/rfkill/rfkill5" }) {
            QVERIFY(writeValue(rfkillSwitch(QLatin1String(dir)) + QStringLiteral("/soft"), "1"));
        }
        QCOMPARE(errorOf(start(QStringLiteral("SetPowered"), { true })), QString());
        QVERIFY(adapterInUse().value(QStringLiteral("powered")).toBool());
        QCOMPARE(takeUnblocked(), QList<int> { 3 });
    }

    void powerRefused()
    {
        fake->run([this] { fake->adapters[hci0].power = FakeBluez::Adapter::Refuses; });
        QDBusPendingCall off = start(QStringLiteral("SetPowered"), { false });
        QCOMPARE(errorOf(off), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(off.error().message(), QStringLiteral("Bluetooth couldn't be turned off."));
        QVERIFY(adapterInUse().value(QStringLiteral("powered")).toBool());
    }

    void powerThatNeverHappensFails()
    {
        fake->run([this] {
            fake->setPowered(hci0, false);
            fake->adapters[hci0].power = FakeBluez::Adapter::Never;
        });
        QTRY_VERIFY(!adapterInUse().value(QStringLiteral("powered")).toBool());
        QDBusPendingCall on = start(QStringLiteral("SetPowered"), { true });
        QCOMPARE(errorOf(on), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(on.error().message(), QStringLiteral("Bluetooth couldn't be turned on."));
        // Three retries after the first.
        QCOMPARE(fakeWrites().count(QStringLiteral("/org/bluez/hci0 Powered=true")), 4);
    }

    void hardBlockIsReported()
    {
        fake->run([this] { fake->setPowered(hci0, false); });
        QTRY_VERIFY(!adapterInUse().value(QStringLiteral("powered")).toBool());
        QVERIFY(writeValue(rfkillSwitch(QStringLiteral("class/bluetooth/hci0/rfkill3")) + QStringLiteral("/soft"), "1"));
        QVERIFY(writeValue(rfkillSwitch(QStringLiteral("class/bluetooth/hci0/rfkill3")) + QStringLiteral("/hard"), "1"));
        QDBusPendingCall on = start(QStringLiteral("SetPowered"), { true });
        QCOMPARE(errorOf(on), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(on.error().message(), QStringLiteral("Bluetooth is turned off by a switch or key on this PC."));
        QCOMPARE(takeUnblocked(), QList<int>());
        QVERIFY(fakeWrites().isEmpty());
    }

    // No adapter at all, a soft-blocked platform switch: the switch is
    // unblocked, and the adapter that appears is turned on.
    void platformSwitchWithoutAnAdapter()
    {
        QVERIFY(writeValue(rfkillSwitch(QStringLiteral("class/rfkill/rfkill5")) + QStringLiteral("/soft"), "1"));
        QVERIFY(writeValue(rfkillSwitch(QStringLiteral("class/bluetooth/hci1/rfkill7")) + QStringLiteral("/soft"), "1"));
        fake->run([this] { fake->removeAdapter(hci0); });
        QTRY_VERIFY(!property(QStringLiteral("Available")).toBool());
        QTRY_VERIFY(property(QStringLiteral("PlatformBlocked")).toBool());
        QCOMPARE(errorOf(start(QStringLiteral("SetPowered"), { true })), QString());
        QCOMPARE(takeUnblocked(), QList<int> { 5 });
        FakeBluez::Adapter off { QStringLiteral("AA:AA:AA:AA:AA:00"), QStringLiteral("vela-pc") };
        off.powered = false;
        fake->run([&] { fake->addAdapter(hci0, off); });
        QTRY_VERIFY(property(QStringLiteral("Available")).toBool());
        QVERIFY(!property(QStringLiteral("PlatformBlocked")).toBool());
        QTRY_VERIFY(adapterInUse().value(QStringLiteral("powered")).toBool());
        // Without a blocked platform switch, there's nothing to turn on.
        fake->run([this] { fake->removeAdapter(hci0); });
        QVERIFY(writeValue(rfkillSwitch(QStringLiteral("class/rfkill/rfkill5")) + QStringLiteral("/soft"), "0"));
        QTRY_VERIFY(!property(QStringLiteral("Available")).toBool());
        QCOMPARE(errorOf(start(QStringLiteral("SetPowered"), { true })), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(takeUnblocked(), QList<int>());
        fake->run([&] { fake->addAdapter(hci0, { QStringLiteral("AA:AA:AA:AA:AA:00"), QStringLiteral("vela-pc") }); });
    }

    // --------------------------------------------------- who may call --

    // dbus-send is an app like any other: it reads, acts, but can't draw
    // Vela's questions.
    void otherAppsCannotPromptForVela()
    {
        auto send = [this](const QStringList& arguments) {
            QProcess process;
            process.start(QStringLiteral("dbus-send"),
                QStringList { QStringLiteral("--bus=") + sessionAddress, QStringLiteral("--print-reply"),
                    QStringLiteral("--dest=org.vela.Bluetooth"), objectPath }
                    + arguments);
            process.waitForFinished(10000);
            return QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
        };
        const QString list = send({ QStringLiteral("org.vela.Bluetooth.Devices") });
        QVERIFY2(list.contains(mouse), qPrintable(list));
        const QString prompt = send({ QStringLiteral("org.vela.Bluetooth.Add"), QStringLiteral("objpath:") + mouse, QStringLiteral("boolean:true") });
        QVERIFY2(prompt.contains(QLatin1String("org.vela.Bluetooth.Error.NotAllowed")), qPrintable(prompt));
        QVERIFY(!fakeCalls().contains(QStringLiteral("Pair ") + mouse));
        const QString add = send({ QStringLiteral("org.vela.Bluetooth.Add"), QStringLiteral("objpath:") + mouse, QStringLiteral("boolean:false") });
        QVERIFY2(!add.contains(QLatin1String("Error")), qPrintable(add));
        // This test is the UI the test build trusts.
        QCOMPARE(errorOf(start(QStringLiteral("Add"), { object(keyboard), true })), QString());
    }

signals:
    void devicesChanged();
};

QTEST_GUILESS_MAIN(BluetoothDaemon)
#include "daemon.moc"
