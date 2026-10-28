// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Vela.Bluetooth's model (common/bluetooth, docs/bluetooth.md §8, §11)
// against the real vela-bluetooth on a fake BlueZ: the lists, the switch
// shown at once and turned back when BlueZ refuses, an Add with its
// question, refusals in plain words, and the daemon missing, leaving and
// coming back. This test is a UI the test build of the daemon trusts.

#include "bluetooth/bluetooth.h"
#include "harness.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QThread>

#include <memory>

using vela::bluetooth::Bluetooth;

namespace {

const QString service = QStringLiteral("org.vela.Bluetooth");
const QString hci0 = QStringLiteral("hci0");

QStringList names(const QVariantList& devices)
{
    QStringList list;
    for (const QVariant& device : devices) {
        list << device.toMap().value(QStringLiteral("name")).toString();
    }
    return list;
}

QVariantMap byName(const QVariantList& devices, const QString& name)
{
    for (const QVariant& device : devices) {
        if (device.toMap().value(QStringLiteral("name")) == name) {
            return device.toMap();
        }
    }
    return {};
}

} // namespace

// An old owner deliberately replies to GetAll *after* handing its name to
// the new daemon. This exercises the client generation guard independently
// of BlueZ's own restart timing.
class DelayedBluetoothOwner : public QDBusVirtualObject {
public:
    QString bus;
    bool getAllCalled = false;

    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection&) override
    {
        if (message.member() == QLatin1String("GetAll")) {
            getAllCalled = true;
            message.setDelayedReply(true);
            QTimer::singleShot(1500, this, [this, message] {
                const QVariantMap stale { { QStringLiteral("Available"), false },
                    { QStringLiteral("Active"), false } };
                QDBusConnection(bus).send(message.createReply(QVariant::fromValue(stale)));
            });
            return true;
        }
        QDBusConnection(bus).send(message.createErrorReply(
            QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"), QStringLiteral("delayed stub")));
        return true;
    }
};

class BluetoothClient : public QObject, protected BluetoothBench {
    Q_OBJECT

public:
    BluetoothClient()
        : BluetoothBench(QStringLiteral("client"))
    {
    }

private:
    QString mouse;
    QString headset;
    QString keyboard;

    // hci0 on, a mouse nearby, a headset paired and connected, a keyboard
    // without a signal strength.
    void resetFake()
    {
        fake->run([this] {
            fake->forgetAgents();
            for (const QString& path : fake->devices.keys()) {
                fake->removeDevice(path);
            }
            for (const QString& name : fake->adapters.keys()) {
                fake->removeAdapter(name);
            }
            fake->addAdapter(hci0, { QStringLiteral("AA:AA:AA:AA:AA:00"), QStringLiteral("vela-pc") });
            FakeBluez::Device device;
            device.adapter = hci0;
            device.address = QStringLiteral("00:11:22:33:44:01");
            device.alias = QStringLiteral("Mouse");
            device.icon = QStringLiteral("input-mouse");
            device.rssi = -50;
            mouse = fake->addDevice(device);
            device.address = QStringLiteral("00:11:22:33:44:02");
            device.alias = QStringLiteral("Headset");
            device.icon = QStringLiteral("audio-headset");
            device.rssi = 0;
            device.paired = device.trusted = device.connected = true;
            device.battery = 80;
            headset = fake->addDevice(device);
            device = {};
            device.adapter = hci0;
            device.address = QStringLiteral("00:11:22:33:44:03");
            device.alias = QStringLiteral("Keyboard");
            device.icon = QStringLiteral("input-keyboard");
            keyboard = fake->addDevice(device);
            fake->calls.clear();
            fake->writes.clear();
        });
    }

    template<typename Change>
    void onDevice(const QString& path, Change change)
    {
        fake->run([&] { change(fake->devices[path]); });
    }

    // A client, as Settings makes it, once it has read the daemon.
    std::unique_ptr<Bluetooth> client()
    {
        auto bluetooth = std::make_unique<Bluetooth>(bus(), true);
        [&] { QTRY_COMPARE(bluetooth->devices().size(), 3); }();
        return bluetooth;
    }

private slots:
    void initTestCase()
    {
        const QString error = setUp();
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void cleanupTestCase() { tearDown(); }

    void init()
    {
        sessionInFront();
        resetFake();
        QVERIFY(startDaemon());
    }

    void cleanup() { stopDaemon(); }

    // Without the daemon, the model is empty and the switch can't move.
    void daemonMissing()
    {
        stopDaemon();
        Bluetooth bluetooth(bus(), true);
        QTest::qWait(100);
        QVERIFY(!bluetooth.available());
        QVERIFY(!bluetooth.platformBlocked());
        QVERIFY(bluetooth.devices().isEmpty());
        QVERIFY(bluetooth.adapter().isEmpty());
        QVERIFY(!bluetooth.powered());
        bluetooth.setPowered(true);
        QVERIFY(!bluetooth.powered());
    }

    // Paired ones connected first; found ones nearest first, then those
    // without a signal strength.
    void theLists()
    {
        auto bluetooth = client();
        QTRY_VERIFY(bluetooth->available());
        QTRY_VERIFY(bluetooth->powered());
        QCOMPARE(bluetooth->adapter().value(QStringLiteral("name")).toString(), QStringLiteral("vela-pc"));
        QCOMPARE(bluetooth->adapters().size(), 1);
        QCOMPARE(names(bluetooth->paired()), QStringList { QStringLiteral("Headset") });
        QCOMPARE(names(bluetooth->found()), (QStringList { QStringLiteral("Mouse"), QStringLiteral("Keyboard") }));
        const QVariantMap headsetEntry = byName(bluetooth->paired(), QStringLiteral("Headset"));
        QCOMPARE(headsetEntry.value(QStringLiteral("battery")).toInt(), 80);
        QCOMPARE(headsetEntry.value(QStringLiteral("type")).toString(), QStringLiteral("headset"));
        QVERIFY(headsetEntry.value(QStringLiteral("connected")).toBool());
        QCOMPARE(byName(bluetooth->found(), QStringLiteral("Keyboard")).value(QStringLiteral("battery")).toInt(), -1);

        // Followed as BlueZ changes: a nearer device goes first.
        fake->run([this] { fake->setDevice(keyboard, QStringLiteral("RSSI"), QVariant::fromValue(qint16(-40))); });
        QTRY_COMPARE(names(bluetooth->found()), (QStringList { QStringLiteral("Keyboard"), QStringLiteral("Mouse") }));
        // A paired one before it by name, but not connected, goes after.
        fake->run([this] {
            fake->setDevice(keyboard, QStringLiteral("Alias"), QStringLiteral("Arc keyboard"));
            fake->setDevice(keyboard, QStringLiteral("Paired"), true);
        });
        QTRY_COMPARE(names(bluetooth->paired()), (QStringList { QStringLiteral("Headset"), QStringLiteral("Arc keyboard") }));
        fake->run([this] { fake->setDevice(headset, QStringLiteral("Connected"), false); });
        QTRY_COMPARE(names(bluetooth->paired()), (QStringList { QStringLiteral("Arc keyboard"), QStringLiteral("Headset") }));
        fake->run([this] { fake->removeDevice(keyboard); });
        QTRY_COMPARE(names(bluetooth->paired()), QStringList { QStringLiteral("Headset") });
    }

    // The switch moves at once, stays while BlueZ works, and goes back
    // when BlueZ refuses, with BlueZ's reason in plain words.
    void theSwitchShowsAtOnce()
    {
        auto bluetooth = client();
        QTRY_VERIFY(bluetooth->powered());
        bluetooth->setPowered(false);
        QVERIFY(!bluetooth->powered());
        QTRY_VERIFY(!bluetooth->adapter().value(QStringLiteral("powered")).toBool());
        QVERIFY(!bluetooth->powered());

        fake->run([this] { fake->adapters[hci0].power = FakeBluez::Adapter::Refuses; });
        bluetooth->setPowered(true);
        QVERIFY(bluetooth->powered());
        QTRY_VERIFY_WITH_TIMEOUT(!bluetooth->powered(), 5000);
        QVERIFY(!bluetooth->error().isEmpty());
    }

    // An Add draws its question here: the code, the device waiting, and
    // added() once paired.
    void anAddWithItsQuestion()
    {
        auto bluetooth = client();
        onDevice(mouse, [](FakeBluez::Device& device) { device.method = FakeBluez::Device::Confirm; });
        QSignalSpy added(bluetooth.get(), &Bluetooth::added);
        bluetooth->add(mouse);
        QCOMPARE(bluetooth->pairing(), mouse);
        QTRY_VERIFY(!bluetooth->question().isEmpty());
        const QVariantMap question = bluetooth->question();
        QCOMPARE(question.value(QStringLiteral("kind")).toString(), QStringLiteral("confirm-passkey"));
        QCOMPARE(question.value(QStringLiteral("drawnBy")).toString(), QStringLiteral("settings"));
        QCOMPARE(question.value(QStringLiteral("passkey")).toString(), QStringLiteral("123456"));
        QCOMPARE(question.value(QStringLiteral("device")).toString(), mouse);
        QVERIFY(!question.value(QStringLiteral("unsolicited")).toBool());
        // BlueZ gives up 60 s after the request.
        const double left = question.value(QStringLiteral("deadline")).toDouble() - double(QDateTime::currentMSecsSinceEpoch());
        QVERIFY2(left > 50000 && left <= 61000, qPrintable(QString::number(left)));
        QTRY_VERIFY(byName(bluetooth->found(), QStringLiteral("Mouse")).value(QStringLiteral("waiting")).toBool());

        QSignalSpy closed(bluetooth.get(), &Bluetooth::questionClosed);
        bluetooth->answer(question.value(QStringLiteral("id")).toString(), QStringLiteral("accept"));
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(closed.at(0).at(1).toString(), QStringLiteral("answered"));
        QVERIFY(bluetooth->question().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(added.size(), 1, 10000);
        QCOMPARE(added.at(0).at(0).toString(), mouse);
        QVERIFY(bluetooth->pairing().isEmpty());
        QVERIFY(byName(bluetooth->paired(), QStringLiteral("Mouse")).value(QStringLiteral("connected")).toBool());
        QVERIFY(bluetooth->error().isEmpty());
    }

    // Cancel while the question is open: it closes, and the user's own
    // choice gives no message.
    void cancellingAnAdd()
    {
        auto bluetooth = client();
        onDevice(mouse, [](FakeBluez::Device& device) { device.method = FakeBluez::Device::EnterPasskey; });
        QSignalSpy added(bluetooth.get(), &Bluetooth::added);
        bluetooth->add(mouse);
        QTRY_COMPARE(bluetooth->question().value(QStringLiteral("kind")).toString(), QStringLiteral("enter-passkey"));
        bluetooth->cancel(mouse);
        QTRY_VERIFY(bluetooth->question().isEmpty());
        QTRY_VERIFY(bluetooth->pairing().isEmpty());
        QTest::qWait(100);
        QCOMPARE(added.size(), 0);
        QVERIFY(bluetooth->error().isEmpty());
        const QVariantMap entry = byName(bluetooth->found(), QStringLiteral("Mouse"));
        QVERIFY(entry.value(QStringLiteral("message")).toString().isEmpty());
        QVERIFY(entry.value(QStringLiteral("operation")).toString().isEmpty());
    }

    // A second Add while one pairs is refused, naming the first device.
    void oneAddAtATime()
    {
        auto bluetooth = client();
        onDevice(mouse, [](FakeBluez::Device& device) { device.pairHangs = true; });
        bluetooth->add(mouse);
        QTRY_COMPARE(byName(bluetooth->found(), QStringLiteral("Mouse")).value(QStringLiteral("operation")).toString(),
            QStringLiteral("pairing"));
        bluetooth->add(keyboard);
        QTRY_COMPARE(bluetooth->error(), QStringLiteral("Wait for Mouse to finish pairing."));
        bluetooth->cancel(mouse);
        QTRY_VERIFY(bluetooth->pairing().isEmpty());
    }

    // A failed step is the device's: its entry has the message, error()
    // stays empty.
    void aFailureIsTheDevices()
    {
        auto bluetooth = client();
        fake->run([this] {
            fake->setDevice(headset, QStringLiteral("Connected"), false);
            fake->devices[headset].connectError = QStringLiteral("org.bluez.Error.Failed");
            fake->devices[headset].connectMessage = QStringLiteral("Host is down");
        });
        QTRY_VERIFY(!byName(bluetooth->paired(), QStringLiteral("Headset")).value(QStringLiteral("connected")).toBool());
        bluetooth->connectDevice(headset);
        QTRY_VERIFY(!byName(bluetooth->paired(), QStringLiteral("Headset")).value(QStringLiteral("message")).toString().isEmpty());
        QTest::qWait(100);
        QVERIFY(bluetooth->error().isEmpty());
    }

    // A name the daemon refuses says why.
    void aRefusedName()
    {
        auto bluetooth = client();
        bluetooth->rename(headset, QStringLiteral("bad\nname"));
        QTRY_VERIFY(bluetooth->error().contains(QLatin1String("248")));
        bluetooth->clearError();
        bluetooth->rename(headset, QStringLiteral("Cuffie"));
        QTRY_VERIFY(!byName(bluetooth->paired(), QStringLiteral("Cuffie")).isEmpty());
        QVERIFY(bluetooth->error().isEmpty());
    }

    // The daemon crashing empties the model and drops its question, which
    // it couldn't close; when it comes back, the model is read again.
    void theDaemonLeavesAndComesBack()
    {
        auto bluetooth = client();
        onDevice(mouse, [](FakeBluez::Device& device) { device.method = FakeBluez::Device::Confirm; });
        bluetooth->add(mouse);
        QTRY_VERIFY(!bluetooth->question().isEmpty());
        QSignalSpy closed(bluetooth.get(), &Bluetooth::questionClosed);
        daemon.kill();
        QVERIFY(daemon.waitForFinished());
        QTRY_VERIFY(!bluetooth->available());
        QVERIFY(bluetooth->devices().isEmpty());
        QVERIFY(bluetooth->question().isEmpty());
        QCOMPARE(closed.size(), 1);
        QTRY_VERIFY(bluetooth->pairing().isEmpty());

        QVERIFY(startDaemon());
        QTRY_VERIFY(bluetooth->available());
        QTRY_COMPARE(bluetooth->devices().size(), 3);
        QVERIFY(bluetooth->powered());
    }

    void staleGetAllIsIgnoredAfterDaemonReplacement()
    {
        stopDaemon();
        auto* old = new DelayedBluetoothOwner;
        old->bus = QStringLiteral("stale-bluetooth-owner");
        old->moveToThread(&fakeThread);
        bool registered = false;
        fake->run([&] {
            auto connection = QDBusConnection::connectToBus(sessionAddress, old->bus);
            registered = connection.registerService(service)
                && connection.registerVirtualObject(QStringLiteral("/org/vela/Bluetooth"), old);
        });
        QVERIFY(registered);
        auto bluetooth = std::make_unique<Bluetooth>(bus(), true);
        bool requested = false;
        QTRY_VERIFY([&] {
            fake->run([&] { requested = old->getAllCalled; });
            return requested;
        }());
        fake->run([&] {
            QDBusConnection(old->bus).unregisterService(service);
        });
        QVERIFY(startDaemon());
        QTRY_VERIFY(bluetooth->available());
        QTRY_COMPARE(bluetooth->devices().size(), 3);
        // This is after the previous owner's delayed false-Available reply.
        QTest::qWait(1800);
        QVERIFY(bluetooth->available());
        QCOMPARE(bluetooth->devices().size(), 3);
        bluetooth.reset();
        fake->run([&] {
            QDBusConnection::disconnectFromBus(old->bus);
            delete old;
        });
    }

    // A lease while "Add a device" is open: the PC is visible, and ending
    // it by hand isn't "Search again".
    void aSearch()
    {
        auto bluetooth = client();
        QSignalSpy search(bluetooth.get(), &Bluetooth::searchChanged);
        bluetooth->startSearch();
        QTRY_VERIFY(bluetooth->searching());
        QVERIFY(!bluetooth->searchEnded());
        QTRY_VERIFY(bluetooth->adapter().value(QStringLiteral("discoverable")).toBool());
        QVERIFY(!bluetooth->searchShared());
        bluetooth->stopSearch();
        QVERIFY(!bluetooth->searching());
        QVERIFY(!bluetooth->searchEnded());
        QTRY_VERIFY(!bluetooth->adapter().value(QStringLiteral("discoverable")).toBool());
        QVERIFY(bluetooth->error().isEmpty());
        QVERIFY(search.size() >= 2);
    }

    // The daemon ends it (the lock): "Search again".
    void aSearchEndedByTheDaemon()
    {
        auto bluetooth = client();
        bluetooth->startSearch();
        QTRY_VERIFY(bluetooth->searching());
        fake->run([this] { logind->set(true, true); });
        QTRY_VERIFY(bluetooth->searchEnded());
        QVERIFY(!bluetooth->searching());
        fake->run([this] { logind->set(true, false); });
        QTRY_VERIFY(bluetooth->active());
        bluetooth->startSearch();
        QTRY_VERIFY(bluetooth->searching());
        QVERIFY(!bluetooth->searchEnded());
        bluetooth->stopSearch();
    }

    // Stopped before the daemon answered: the lease it then gets is given
    // back.
    void aSearchStoppedAtOnce()
    {
        auto bluetooth = client();
        bluetooth->startSearch();
        bluetooth->stopSearch();
        QTest::qWait(300);
        QVERIFY(!bluetooth->searching());
        QTRY_VERIFY(!bluetooth->adapter().value(QStringLiteral("discovering")).toBool());
    }

    // Another app searching: no visibility from Vela, and said so; it then
    // keeps the PC visible after Vela's search, until "Stop being
    // visible".
    void anotherAppSearching()
    {
        auto bluetooth = client();
        QDBusConnection other = QDBusConnection::connectToBus(systemAddress, QStringLiteral("other-searcher"));
        auto discover = [&](bool on) {
            QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), QStringLiteral("/org/bluez/hci0"),
                QStringLiteral("org.bluez.Adapter1"), on ? QStringLiteral("StartDiscovery") : QStringLiteral("StopDiscovery"));
            return other.call(call, QDBus::Block, 5000).type() == QDBusMessage::ReplyMessage;
        };
        QVERIFY(discover(true));
        QTRY_VERIFY(bluetooth->adapter().value(QStringLiteral("discovering")).toBool());
        bluetooth->startSearch();
        QTRY_VERIFY(bluetooth->searchShared());
        QVERIFY(!bluetooth->adapter().value(QStringLiteral("discoverable")).toBool());
        bluetooth->stopSearch();
        QTRY_VERIFY(!bluetooth->searchShared());
        QVERIFY(discover(false));
        QTRY_VERIFY(!bluetooth->adapter().value(QStringLiteral("discovering")).toBool());

        // Vela first, the other app during Vela's search.
        bluetooth->startSearch();
        QTRY_VERIFY(bluetooth->adapter().value(QStringLiteral("discoverable")).toBool());
        QVERIFY(discover(true));
        bluetooth->stopSearch();
        QTRY_VERIFY_WITH_TIMEOUT(bluetooth->stillVisible(), 4000);
        bluetooth->stopBeingVisible();
        QTRY_VERIFY(!bluetooth->stillVisible());
        QTRY_VERIFY(!bluetooth->adapter().value(QStringLiteral("discoverable")).toBool());
        QVERIFY(discover(false));
        other = QDBusConnection(QString());
        QDBusConnection::disconnectFromBus(QStringLiteral("other-searcher"));
    }
};

QTEST_GUILESS_MAIN(BluetoothClient)
#include "client.moc"
