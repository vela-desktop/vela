// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-bluetooth against a fake BlueZ and a fake logind (docs/bluetooth.md
// §11): the model, the adapter in use, each operation and the ways it ends
// early, power and rfkill, the agent and its questions, concurrent
// requests, other desktop sessions, and who may call. The real daemon
// binary runs on two private buses, a fake /dev/rfkill and a fake /sys;
// this test is the UI the test build trusts.

#include "fakebluez.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
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

// One Question, or one entry of Questions().
struct Asked {
    qulonglong id = 0;
    QString device;
    QVariantMap details;

    QString kind() const { return details.value(QStringLiteral("kind")).toString(); }
};

const QString velaAgent = QStringLiteral("/org/vela/BluetoothAgent");
const QString a2dpSink = QStringLiteral("0000110b-0000-1000-8000-00805f9b34fb");

} // namespace

Q_DECLARE_METATYPE(DeviceMap)

class BluetoothDaemon : public QObject, protected QDBusContext {
    Q_OBJECT

    QProcess systemBus;
    QProcess sessionBus;
    QString systemAddress;
    QString sessionAddress;
    QThread fakeThread;
    FakeBluez* fake = nullptr;
    FakeLogind* logind = nullptr;
    QTemporaryDir home;
    QProcess daemon;
    QList<Operation> operations;
    QList<Asked> asked; // the Question signals
    QList<QPair<qulonglong, QVariantMap>> questionChanges;
    QList<QPair<qulonglong, QString>> closed; // QuestionClosed: id, reason
    QList<QPair<qulonglong, QString>> searchesEnded; // SearchEnded: lease, reason
    int searchLimitMs = 0; // a lease's time limit, if the test shortens it
    int restartWindowMs = 0; // bounded retry-window tests
    bool stoppedBeingVisible = false; // the test pressed "Stop being visible"
    QString daemonName; // its unique name: signals of an earlier daemon don't count
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

    bool fromDaemon() const { return calledFromDBus() && message().service() == daemonName; }

    QStringList agentLog()
    {
        QStringList log;
        fake->run([&] { log = fake->agentLog; });
        return log;
    }

    QString defaultAgent()
    {
        QString agent;
        fake->run([&] { agent = fake->defaultAgent(); });
        return agent;
    }

    bool velaIsTheDefaultAgent() { return defaultAgent().endsWith(u' ' + velaAgent); }

    // The device asks by itself, through the default agent.
    void deviceAsks(const QString& path, const QString& method, const QVariantList& extra = {})
    {
        fake->run([&] { fake->ask(path, method, extra); });
    }

    void setSession(bool active, bool locked)
    {
        fake->run([&] { logind->set(active, locked); });
    }

    static QList<Asked> questionsOf(const QDBusMessage& reply)
    {
        QList<Asked> list;
        const QDBusArgument argument = reply.arguments().value(0).value<QDBusArgument>();
        argument.beginArray();
        while (!argument.atEnd()) {
            Asked entry;
            QDBusObjectPath device;
            argument.beginStructure();
            argument >> entry.id >> device >> entry.details;
            argument.endStructure();
            entry.device = device.path();
            list << entry;
        }
        argument.endArray();
        return list;
    }

    QList<Asked> openQuestions() const { return questionsOf(call(QStringLiteral("Questions"))); }

    // The next Question signal after `seen` of them.
    Asked nextQuestion(qsizetype seen)
    {
        [&] { QTRY_VERIFY_WITH_TIMEOUT(asked.size() > seen, 10000); }();
        return asked.size() > seen ? asked.at(seen) : Asked {};
    }

    QString answer(qulonglong id, const QString& response, const QString& value = {}) const
    {
        return errorOf(call(QStringLiteral("Answer"), { id, response, value }));
    }

    QString closedReason(qulonglong id) const
    {
        for (const auto& [closedId, reason] : closed) {
            if (closedId == id) {
                return reason;
            }
        }
        return {};
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
        // The last test's signals may still be queued here (blocking calls
        // don't process events): from now on they don't count.
        daemonName.clear();
        operations.clear();
        asked.clear();
        questionChanges.clear();
        closed.clear();
        searchesEnded.clear();
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("DBUS_SYSTEM_BUS_ADDRESS"), systemAddress);
        environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), sessionAddress);
        environment.insert(QStringLiteral("VELA_RFKILL"), home.filePath(QStringLiteral("rfkill")));
        environment.insert(QStringLiteral("VELA_SYSFS"), home.filePath(QStringLiteral("sys")));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home.filePath(QStringLiteral("config")));
        // When logind can't name the session, as in a container.
        environment.insert(QStringLiteral("XDG_SESSION_ID"), QStringLiteral("fake"));
        if (searchLimitMs > 0) {
            environment.insert(QStringLiteral("VELA_BT_SEARCH_LIMIT_MS"), QString::number(searchLimitMs));
        }
        if (restartWindowMs > 0) {
            environment.insert(QStringLiteral("VELA_BT_RESTART_WINDOW_MS"), QString::number(restartWindowMs));
        }
        daemon.setProcessEnvironment(environment);
        daemon.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        daemon.start(QStringLiteral(VELA_BLUETOOTH_DAEMON), {});
        if (!daemon.waitForStarted()) {
            return false;
        }
        QDBusConnection bus = client();
        for (int i = 0; i < 500 && !bus.interface()->isServiceRegistered(service); ++i) {
            QTest::qWait(20);
        }
        daemonName = bus.interface()->serviceOwner(service);
        // As Vela's UI does: from now on the question signals come here.
        return !daemonName.isEmpty() && errorOf(call(QStringLiteral("Questions"))).isEmpty();
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
            fake->forgetAgents();
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

    // -------------------------------------------------------- searches --

    // StartSearch: the lease, or 0 with the error in `error`.
    qulonglong startSearch(bool discoverable, QString* error = nullptr, const QDBusConnection& bus = QDBusConnection(QStringLiteral("client")))
    {
        const QDBusMessage reply = bus.call(method(QStringLiteral("StartSearch"), { discoverable }), QDBus::Block, 5000);
        if (error) {
            *error = errorOf(reply);
        }
        return reply.type() == QDBusMessage::ReplyMessage ? reply.arguments().value(0).toULongLong() : 0;
    }

    QString searchEndReason(qulonglong lease) const
    {
        for (const auto& [id, reason] : searchesEnded) {
            if (id == lease) {
                return reason;
            }
        }
        return {};
    }

    // The clients discovering on hci0, and whether it is visible.
    QStringList discovering() const
    {
        QStringList clients;
        fake->run([&] { clients = fake->adapters.value(hci0).discovering; });
        return clients;
    }

    bool discoverable() const
    {
        bool visible = false;
        fake->run([&] { visible = fake->adapters.value(hci0).discoverable; });
        return visible;
    }

    // Another client of BlueZ (a command-line tool, another desktop)
    // discovering on hci0, without a filter.
    void otherDiscovers(bool on)
    {
        QDBusConnection other = QDBusConnection::connectToBus(systemAddress, QStringLiteral("other-searcher"));
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), QStringLiteral("/org/bluez/hci0"),
            QStringLiteral("org.bluez.Adapter1"), on ? QStringLiteral("StartDiscovery") : QStringLiteral("StopDiscovery"));
        QCOMPARE(other.call(call, QDBus::Block, 5000).type(), QDBusMessage::ReplyMessage);
        if (!on) {
            QDBusConnection::disconnectFromBus(QStringLiteral("other-searcher"));
        }
    }

    template<typename Change>
    void onDevice(const QString& path, Change change)
    {
        fake->run([&] { change(fake->devices[path]); });
    }

private slots:
    void onOperation(const QDBusObjectPath& device, const QString& state, const QString& error, const QString& message)
    {
        if (fromDaemon()) {
            operations << Operation { device.path(), state, error, message };
        }
    }

    void onQuestion(qulonglong id, const QDBusObjectPath& device, const QVariantMap& details)
    {
        if (fromDaemon()) {
            asked << Asked { id, device.path(), details };
        }
    }

    void onQuestionChanged(qulonglong id, const QVariantMap& details)
    {
        if (fromDaemon()) {
            questionChanges << qMakePair(id, details);
        }
    }

    void onSearchEnded(qulonglong lease, const QString& reason)
    {
        if (fromDaemon()) {
            searchesEnded << qMakePair(lease, reason);
        }
    }

    void onQuestionClosed(qulonglong id, const QString& reason)
    {
        if (fromDaemon()) {
            closed << qMakePair(id, reason);
        }
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
        QVERIFY(bus.connect(service, objectPath, service, QStringLiteral("Question"), this,
            SLOT(onQuestion(qulonglong, QDBusObjectPath, QVariantMap))));
        QVERIFY(bus.connect(service, objectPath, service, QStringLiteral("QuestionChanged"), this,
            SLOT(onQuestionChanged(qulonglong, QVariantMap))));
        QVERIFY(bus.connect(service, objectPath, service, QStringLiteral("QuestionClosed"), this,
            SLOT(onQuestionClosed(qulonglong, QString))));
        QVERIFY(bus.connect(service, objectPath, service, QStringLiteral("SearchEnded"), this,
            SLOT(onSearchEnded(qulonglong, QString))));

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
        QVERIFY(ok);
    }

    void cleanupTestCase()
    {
        fake->run([this] {
            QDBusConnection::disconnectFromBus(fake->bus);
            QDBusConnection::disconnectFromBus(logind->bus);
        });
        QMetaObject::invokeMethod(fake, [this] { delete logind; delete fake; }, Qt::BlockingQueuedConnection);
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
        fake->run([this] {
            fake->comeBack();
            logind->active = true;
            logind->locked = false;
            logind->getAllErrors = 0;
            logind->getAllMalformed = 0;
            fake->managedErrors = 0;
            fake->managedMalformed = 0;
            fake->registerErrors = 0;
            fake->defaultErrors = 0;
            fake->removeDoesNotExist = false;
        });
        searchLimitMs = 0;
        restartWindowMs = 0;
        stoppedBeingVisible = false;
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
        QTRY_VERIFY(property(QStringLiteral("Active")).toBool());
        QTRY_VERIFY(velaIsTheDefaultAgent());
        // The daemon test acts as the future B4 shell presenter. A normal
        // Settings client does not opt in, so spontaneous pairing is refused.
        QCOMPARE(errorOf(call(QStringLiteral("PresentQuestions"))), QString());
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
            QTRY_VERIFY_WITH_TIMEOUT(openQuestions().isEmpty(), 5000);
            // No lease, and no discovery of Vela's.
            QTRY_COMPARE(property(QStringLiteral("Search")).toString(), QString());
            QTRY_VERIFY_WITH_TIMEOUT(discovering().isEmpty(), 5000);
        }
        // Vela never changes adapter-wide settings on its own (§6.7):
        // Discoverable only on the user's "Stop being visible" (§6.6).
        for (const QString& write : fakeWrites()) {
            for (const char* name : { " Discoverable=", " DiscoverableTimeout=", " Pairable=", " PairableTimeout=" }) {
                if (stoppedBeingVisible && write == QStringLiteral("/org/bluez/hci0 Discoverable=false")) {
                    continue;
                }
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
        // BlueZ came back without agents: Vela's is registered again.
        QTRY_VERIFY(velaIsTheDefaultAgent());
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

    void failedBluezSnapshotIsRetried()
    {
        fake->run([this] {
            fake->managedErrors = 1;
            QVERIFY(fake->leave());
            QVERIFY(fake->comeBack());
        });
        QTRY_COMPARE(devices().size(), 3);
        QTRY_VERIFY(fakeCalls().count(QStringLiteral("GetManagedObjects")) >= 2);
    }

    void malformedBluezSnapshotIsRetried()
    {
        fake->run([this] {
            fake->managedMalformed = 1;
            QVERIFY(fake->leave());
            QVERIFY(fake->comeBack());
        });
        QTRY_COMPARE(devices().size(), 3);
    }

    void failedAgentRegistrationIsRetried()
    {
        fake->run([this] {
            fake->registerErrors = 1;
            QVERIFY(fake->leave());
            QVERIFY(fake->comeBack());
        });
        QTRY_VERIFY(velaIsTheDefaultAgent());
    }

    void failedDefaultAgentRequestIsRetried()
    {
        setSession(false, false);
        QTRY_VERIFY(defaultAgent().isEmpty());
        fake->run([this] { fake->defaultErrors = 1; });
        setSession(true, false);
        QTRY_VERIFY(velaIsTheDefaultAgent());
    }

    void logindReadFailureKeepsSessionInactiveUntilRecovered()
    {
        QCOMPARE(stopDaemon(), 0);
        fake->run([this] { logind->getAllErrors = 1; });
        QVERIFY(startDaemon());
        QTRY_VERIFY(!property(QStringLiteral("Active")).toBool());
        QTRY_VERIFY(property(QStringLiteral("Active")).toBool());
        QTRY_VERIFY(velaIsTheDefaultAgent());
        QCOMPARE(errorOf(call(QStringLiteral("PresentQuestions"))), QString());
    }

    void invalidLogindReplyKeepsSessionInactiveUntilRecovered()
    {
        QCOMPARE(stopDaemon(), 0);
        fake->run([this] { logind->getAllMalformed = 1; });
        QVERIFY(startDaemon());
        QTRY_VERIFY(!property(QStringLiteral("Active")).toBool());
        QTRY_VERIFY(property(QStringLiteral("Active")).toBool());
        QCOMPARE(errorOf(call(QStringLiteral("PresentQuestions"))), QString());
    }

    void unsolicitedRequestsWithoutPresenterAreRejected()
    {
        QCOMPARE(stopDaemon(), 0);
        QVERIFY(startDaemon()); // No PresentQuestions after this restart.
        QTRY_VERIFY(velaIsTheDefaultAgent());
        deviceAsks(mouse, QStringLiteral("RequestConfirmation"), { 123u });
        QTRY_VERIFY(agentLog().contains(QStringLiteral("RequestConfirmation ") + mouse
            + QStringLiteral(" -> org.bluez.Error.Rejected")));
        QVERIFY(openQuestions().isEmpty());
        QCOMPARE(asked.size(), 0);
    }

    void alreadyAbsentDeviceCompletesRemovalImmediately()
    {
        fake->run([this] { fake->removeDoesNotExist = true; });
        QCOMPARE(errorOf(start(QStringLiteral("Remove"), { object(headset) })), QString());
        QTRY_VERIFY(!devices().contains(headset));
    }

    void adapterSelectionFailureDoesNotChangeLiveState()
    {
        fake->run([this] {
            fake->addAdapter(hci1, { QStringLiteral("BB:BB:BB:BB:BB:01"), QStringLiteral("dongle") });
        });
        QTRY_COMPARE(qdbus_cast<QList<QVariantMap>>(property(QStringLiteral("Adapters"))).size(), 2);
        const QString path = home.filePath(QStringLiteral("config/vela/vela.conf"));
        QVERIFY(QDir().mkpath(path)); // rename(temp, directory) must fail.
        const QString oldAddress = adapterInUse().value(QStringLiteral("address")).toString();
        QCOMPARE(errorOf(call(QStringLiteral("UseAdapter"), { QStringLiteral("BB:BB:BB:BB:BB:01") })),
            QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(adapterInUse().value(QStringLiteral("address")).toString(), oldAddress);
        QVERIFY(QDir().rmdir(path));
    }

    void offCancelsDeferredPlatformPowerOn()
    {
        fake->run([this] { fake->removeAdapter(hci0); });
        QTRY_VERIFY(!property(QStringLiteral("Available")).toBool());
        QVERIFY(writeValue(rfkillSwitch(QStringLiteral("class/rfkill/rfkill5")) + QStringLiteral("/soft"), "1"));
        QCOMPARE(errorOf(start(QStringLiteral("SetPowered"), { true })), QString());
        QCOMPARE(errorOf(start(QStringLiteral("SetPowered"), { false })), QString());
        fake->run([this] {
            FakeBluez::Adapter adapter { QStringLiteral("AA:AA:AA:AA:AA:00"), QStringLiteral("vela-pc") };
            adapter.powered = false;
            fake->addAdapter(hci0, adapter);
        });
        QTRY_VERIFY(property(QStringLiteral("Available")).toBool());
        QTest::qWait(200);
        QVERIFY(!adapterInUse().value(QStringLiteral("powered")).toBool());
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
        // BlueZ may report Connect success after the cancelled operation ends.
        // The daemon must compensate, rather than leaving the headset on.
        QTest::qWait(2200);
        QTRY_VERIFY(!devices().value(headset).value(QStringLiteral("connected")).toBool());
        QTRY_VERIFY(fakeCalls().count(QStringLiteral("Disconnect ") + headset) >= 3);
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

    // ---------------------------------------------------- the agent (§6.4) --

    void agentIsRegisteredAsKeyboardDisplay()
    {
        QVERIFY(fakeCalls().contains(QStringLiteral("RegisterAgent KeyboardDisplay")));
        QVERIFY(fakeCalls().contains(QStringLiteral("RequestDefaultAgent")));
    }

    // Settings draws an Add's question when it asked to (promptsHere).
    void confirmPasskeyAccepted()
    {
        onDevice(mouse, [](FakeBluez::Device& device) {
            device.method = FakeBluez::Device::Confirm;
            device.passkey = 4321;
        });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), true });
        const Asked question = nextQuestion(0);
        QCOMPARE(question.kind(), QStringLiteral("confirm-passkey"));
        QCOMPARE(question.device, mouse);
        QCOMPARE(question.details.value(QStringLiteral("passkey")).toUInt(), 4321u);
        QCOMPARE(question.details.value(QStringLiteral("name")).toString(), QStringLiteral("Mouse"));
        QCOMPARE(question.details.value(QStringLiteral("icon")).toString(), QStringLiteral("input-mouse"));
        QCOMPARE(question.details.value(QStringLiteral("address")).toString(), mouseAddress);
        QCOMPARE(question.details.value(QStringLiteral("unsolicited")).toBool(), false);
        QCOMPARE(question.details.value(QStringLiteral("drawnBy")).toString(), QStringLiteral("settings"));
        QVERIFY(question.details.value(QStringLiteral("expires")).toULongLong() > 0);
        // The same question, from Questions().
        const QList<Asked> open = openQuestions();
        QCOMPARE(open.size(), 1);
        QCOMPARE(open.first().id, question.id);
        QCOMPARE(open.first().details, question.details);
        QCOMPARE(answer(question.id, QStringLiteral("accept")), QString());
        QCOMPARE(errorOf(add), QString());
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("answered"));
        QVERIFY(agentLog().contains(QStringLiteral("RequestConfirmation ") + mouse + QStringLiteral(" -> ok")));
        const QVariantMap entry = devices().value(mouse);
        QVERIFY(entry.value(QStringLiteral("paired")).toBool());
        QVERIFY(entry.value(QStringLiteral("connected")).toBool());
        // Answered once: a second answer finds it closed.
        QCOMPARE(answer(question.id, QStringLiteral("accept")), QStringLiteral("org.vela.Bluetooth.Error.QuestionClosed"));
        QCOMPARE(answer(question.id + 1, QStringLiteral("accept")), QStringLiteral("org.vela.Bluetooth.Error.UnknownQuestion"));
        QCOMPARE(answer(0, QStringLiteral("accept")), QStringLiteral("org.vela.Bluetooth.Error.UnknownQuestion"));
    }

    // The user's own "no" ends the Add with no message.
    void confirmPasskeyRejected()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.method = FakeBluez::Device::Confirm; });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        const Asked question = nextQuestion(0);
        QCOMPARE(question.details.value(QStringLiteral("drawnBy")).toString(), QStringLiteral("shell"));
        QCOMPARE(answer(question.id, QStringLiteral("cancel")), QStringLiteral("org.vela.Bluetooth.Error.WrongResponse"));
        QCOMPARE(answer(question.id, QStringLiteral("accept"), QStringLiteral("1")), QStringLiteral("org.vela.Bluetooth.Error.InvalidValue"));
        QCOMPARE(answer(question.id, QStringLiteral("reject")), QString());
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Cancelled"));
        QTRY_VERIFY(agentLog().contains(QStringLiteral("RequestConfirmation ") + mouse + QStringLiteral(" -> org.bluez.Error.Rejected")));
        QTRY_COMPARE(operations.last().state, QStringLiteral("cancelled"));
        QVERIFY(operations.last().error.isEmpty());
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("answered"));
        QVERIFY(!devices().value(mouse).value(QStringLiteral("paired")).toBool());
        QVERIFY(!devices().value(mouse).contains(QStringLiteral("error")));
    }

    void enterPasskey()
    {
        onDevice(mouse, [](FakeBluez::Device& device) {
            device.method = FakeBluez::Device::EnterPasskey;
            device.passkey = 42;
        });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        const Asked question = nextQuestion(0);
        QCOMPARE(question.kind(), QStringLiteral("enter-passkey"));
        QVERIFY(!question.details.contains(QStringLiteral("passkey")));
        QCOMPARE(answer(question.id, QStringLiteral("reject")), QStringLiteral("org.vela.Bluetooth.Error.WrongResponse"));
        QCOMPARE(answer(question.id, QStringLiteral("accept"), QStringLiteral("abc")), QStringLiteral("org.vela.Bluetooth.Error.InvalidValue"));
        QCOMPARE(answer(question.id, QStringLiteral("accept"), QStringLiteral("1000000")), QStringLiteral("org.vela.Bluetooth.Error.InvalidValue"));
        // Still open after the refused answers.
        QCOMPARE(openQuestions().size(), 1);
        QCOMPARE(answer(question.id, QStringLiteral("accept"), QStringLiteral("000042")), QString());
        QCOMPARE(errorOf(add), QString());
        QVERIFY(agentLog().contains(QStringLiteral("RequestPasskey ") + mouse + QStringLiteral(" -> 42")));
        QVERIFY(devices().value(mouse).value(QStringLiteral("paired")).toBool());
    }

    void enterPasskeyCancelled()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.method = FakeBluez::Device::EnterPasskey; });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        const Asked question = nextQuestion(0);
        QCOMPARE(answer(question.id, QStringLiteral("cancel")), QString());
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Cancelled"));
        QTRY_VERIFY(agentLog().contains(QStringLiteral("RequestPasskey ") + mouse + QStringLiteral(" -> org.bluez.Error.Canceled")));
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("CancelPairing ") + mouse));
    }

    // A passkey shown here and typed on the device: the digits typed so
    // far, then it closes once paired.
    void showPasskey()
    {
        onDevice(keyboard, [](FakeBluez::Device& device) {
            device.method = FakeBluez::Device::ShowPasskey;
            device.passkey = 7;
            device.pairDelay = 1500;
        });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(keyboard), false });
        const Asked question = nextQuestion(0);
        QCOMPARE(question.kind(), QStringLiteral("show-passkey"));
        QCOMPARE(question.details.value(QStringLiteral("passkey")).toUInt(), 7u);
        QCOMPARE(question.details.value(QStringLiteral("entered")).toUInt(), 0u);
        QCOMPARE(answer(question.id, QStringLiteral("accept")), QStringLiteral("org.vela.Bluetooth.Error.WrongResponse"));
        QTRY_COMPARE(questionChanges.size(), 1);
        QCOMPARE(questionChanges.first().first, question.id);
        QCOMPARE(questionChanges.first().second, (QVariantMap { { QStringLiteral("entered"), QVariant::fromValue(quint16(3)) } }));
        QCOMPARE(errorOf(add), QString());
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("answered"));
        QCOMPARE(asked.size(), 1);
    }

    // DisplayPinCode: answered at once, the PIN shown until paired.
    void showPin()
    {
        onDevice(keyboard, [](FakeBluez::Device& device) {
            device.method = FakeBluez::Device::ShowPin;
            device.pin = QStringLiteral("4321");
            device.pairDelay = 500;
        });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(keyboard), false });
        const Asked question = nextQuestion(0);
        QCOMPARE(question.kind(), QStringLiteral("show-pin"));
        QCOMPARE(question.details.value(QStringLiteral("pin")).toString(), QStringLiteral("4321"));
        QTRY_VERIFY(agentLog().contains(QStringLiteral("DisplayPinCode ") + keyboard + QStringLiteral(" -> ok")));
        QCOMPARE(errorOf(add), QString());
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("answered"));
    }

    // ------------------------------------------------ legacy PIN (§6.4) --

    void legacyPinOfAKeyboardIsShown()
    {
        onDevice(keyboard, [](FakeBluez::Device& device) { device.method = FakeBluez::Device::LegacyPin; });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(keyboard), false });
        const Asked question = nextQuestion(0);
        QCOMPARE(question.kind(), QStringLiteral("show-pin"));
        const QString pin = question.details.value(QStringLiteral("pin")).toString();
        QVERIFY2(pin.size() == 6 && pin.toUInt() <= 999999 && pin.count(QRegularExpression(QStringLiteral("[0-9]"))) == 6, qPrintable(pin));
        QCOMPARE(errorOf(add), QString());
        // The PIN BlueZ got is the one shown.
        QVERIFY(agentLog().contains(QStringLiteral("RequestPinCode ") + keyboard + QStringLiteral(" -> ") + pin));
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("answered"));
    }

    void legacyPinOfAMouseIs0000()
    {
        onDevice(mouse, [](FakeBluez::Device& device) {
            device.method = FakeBluez::Device::LegacyPin;
            device.pin = QStringLiteral("0000");
        });
        QCOMPARE(errorOf(start(QStringLiteral("Add"), { object(mouse), false })), QString());
        QVERIFY(agentLog().contains(QStringLiteral("RequestPinCode ") + mouse + QStringLiteral(" -> 0000")));
        QVERIFY(asked.isEmpty());
    }

    void legacyPinOfAPhoneIsTyped()
    {
        FakeBluez::Device phoneDevice;
        phoneDevice.adapter = hci0;
        phoneDevice.address = QStringLiteral("00:11:22:33:44:04");
        phoneDevice.alias = QStringLiteral("Phone");
        phoneDevice.icon = QStringLiteral("phone");
        phoneDevice.method = FakeBluez::Device::LegacyPin;
        phoneDevice.pin = QStringLiteral("1234");
        QString phone;
        fake->run([&] { phone = fake->addDevice(phoneDevice); });
        QTRY_VERIFY(devices().contains(phone));
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(phone), false });
        const Asked question = nextQuestion(0);
        QCOMPARE(question.kind(), QStringLiteral("enter-pin"));
        QCOMPARE(answer(question.id, QStringLiteral("accept"), QStringLiteral("12345678901234567")),
            QStringLiteral("org.vela.Bluetooth.Error.InvalidValue"));
        QCOMPARE(answer(question.id, QStringLiteral("accept"), QString()), QStringLiteral("org.vela.Bluetooth.Error.InvalidValue"));
        QCOMPARE(answer(question.id, QStringLiteral("accept"), QStringLiteral("1234")), QString());
        QCOMPARE(errorOf(add), QString());
        QVERIFY(devices().value(phone).value(QStringLiteral("paired")).toBool());
    }

    // A legacy request nobody started: never 0000, the user types it.
    void unsolicitedLegacyPinIsTyped()
    {
        deviceAsks(mouse, QStringLiteral("RequestPinCode"));
        const Asked question = nextQuestion(0);
        QCOMPARE(question.kind(), QStringLiteral("enter-pin"));
        QCOMPARE(question.details.value(QStringLiteral("unsolicited")).toBool(), true);
        QCOMPARE(question.details.value(QStringLiteral("drawnBy")).toString(), QStringLiteral("shell"));
        QCOMPARE(answer(question.id, QStringLiteral("cancel")), QString());
        QTRY_VERIFY(agentLog().contains(QStringLiteral("RequestPinCode ") + mouse + QStringLiteral(" -> org.bluez.Error.Canceled")));
    }

    // ------------------------------------- requests the user didn't start --

    void authorizeService()
    {
        deviceAsks(headset, QStringLiteral("AuthorizeService"), { a2dpSink });
        Asked question = nextQuestion(0);
        QCOMPARE(question.kind(), QStringLiteral("authorize-service"));
        QCOMPARE(question.details.value(QStringLiteral("uuid")).toString(), a2dpSink);
        QCOMPARE(question.details.value(QStringLiteral("service")).toString(), QStringLiteral("audio streaming"));
        QCOMPARE(question.details.value(QStringLiteral("unsolicited")).toBool(), true);
        QCOMPARE(answer(question.id, QStringLiteral("accept")), QString());
        QTRY_VERIFY(agentLog().contains(QStringLiteral("AuthorizeService ") + headset + QStringLiteral(" -> ok")));
        deviceAsks(headset, QStringLiteral("AuthorizeService"), { a2dpSink });
        question = nextQuestion(1);
        QVERIFY(question.id > asked.first().id);
        QCOMPARE(answer(question.id, QStringLiteral("reject")), QString());
        QTRY_VERIFY(agentLog().contains(QStringLiteral("AuthorizeService ") + headset + QStringLiteral(" -> org.bluez.Error.Rejected")));
    }

    // A phone pairs by itself (Just Works from its side).
    void authorizePairing()
    {
        deviceAsks(mouse, QStringLiteral("RequestAuthorization"));
        const Asked question = nextQuestion(0);
        QCOMPARE(question.kind(), QStringLiteral("authorize-pairing"));
        QCOMPARE(answer(question.id, QStringLiteral("accept")), QString());
        QTRY_VERIFY(devices().value(mouse).value(QStringLiteral("paired")).toBool());
    }

    // An unsolicited passkey shown here closes once the device is paired.
    void unsolicitedShowPasskeyClosesWhenPaired()
    {
        deviceAsks(mouse, QStringLiteral("DisplayPasskey"), { 123u, QVariant::fromValue(quint16(0)) });
        const Asked question = nextQuestion(0);
        QCOMPARE(question.kind(), QStringLiteral("show-passkey"));
        deviceAsks(mouse, QStringLiteral("DisplayPasskey"), { 123u, QVariant::fromValue(quint16(5)) });
        QTRY_COMPARE(questionChanges.size(), 1);
        fake->run([this] { fake->setDevice(mouse, QStringLiteral("Paired"), true); });
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("answered"));
    }

    // -------------------------------------------- how a question closes --

    void cancelledByTheDevice()
    {
        deviceAsks(mouse, QStringLiteral("RequestConfirmation"), { 99u });
        const Asked question = nextQuestion(0);
        fake->run([this] { fake->cancelRequestOn(mouse); });
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("cancelled-by-device"));
        // Too late (§6.5, rule 7).
        QCOMPARE(answer(question.id, QStringLiteral("accept")), QStringLiteral("org.vela.Bluetooth.Error.QuestionClosed"));
    }

    void closesWhenTheDeviceGoes()
    {
        deviceAsks(mouse, QStringLiteral("RequestConfirmation"), { 99u });
        const Asked question = nextQuestion(0);
        fake->run([this] { fake->removeDevice(mouse); });
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("device-gone"));
    }

    void closesWhenBluezGoes()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.method = FakeBluez::Device::Confirm; });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        const Asked question = nextQuestion(0);
        fake->run([this] { fake->leave(); });
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("bluetooth-gone"));
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        fake->run([this] { fake->comeBack(); });
        QTRY_VERIFY(velaIsTheDefaultAgent());
    }

    // BlueZ removes an adapter's devices before it.
    void closesWhenTheAdapterGoes()
    {
        deviceAsks(mouse, QStringLiteral("RequestConfirmation"), { 99u });
        const Asked question = nextQuestion(0);
        fake->run([this] { fake->removeAdapter(hci0); });
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("device-gone"));
        fake->run([this] { fake->addAdapter(hci0, { QStringLiteral("AA:AA:AA:AA:AA:00"), QStringLiteral("vela-pc") }); });
    }

    // ------------------------------------------ concurrent requests (§6.5) --

    // A phone asking blocks the Adds of other devices.
    void addWhileADeviceAsks()
    {
        deviceAsks(headset, QStringLiteral("RequestConfirmation"), { 99u });
        const Asked question = nextQuestion(0);
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Busy"));
        QCOMPARE(add.error().message(), headset);
        QVERIFY(!fakeCalls().contains(QStringLiteral("Pair ") + mouse));
        QCOMPARE(answer(question.id, QStringLiteral("reject")), QString());
        // Free again.
        QCOMPARE(errorOf(start(QStringLiteral("Add"), { object(mouse), false })), QString());
    }

    // Add on the device that is asking takes its request over: no Pair.
    void addOnTheDeviceThatAsks()
    {
        deviceAsks(mouse, QStringLiteral("RequestConfirmation"), { 99u });
        const Asked question = nextQuestion(0);
        QCOMPARE(question.details.value(QStringLiteral("unsolicited")).toBool(), true);
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), true });
        const Asked adopted = nextQuestion(1);
        QCOMPARE(adopted.id, question.id);
        QCOMPARE(adopted.details.value(QStringLiteral("unsolicited")).toBool(), false);
        QCOMPARE(adopted.details.value(QStringLiteral("drawnBy")).toString(), QStringLiteral("settings"));
        QTRY_COMPARE(devices().value(mouse).value(QStringLiteral("operation")).toString(), QStringLiteral("pairing"));
        QCOMPARE(answer(question.id, QStringLiteral("accept")), QString());
        QCOMPARE(errorOf(add), QString());
        QVERIFY(!fakeCalls().contains(QStringLiteral("Pair ") + mouse));
        const QVariantMap entry = devices().value(mouse);
        QVERIFY(entry.value(QStringLiteral("paired")).toBool());
        QVERIFY(entry.value(QStringLiteral("trusted")).toBool());
        QVERIFY(entry.value(QStringLiteral("connected")).toBool());
        QTRY_COMPARE(states(mouse),
            (QStringList { QStringLiteral("pairing"), QStringLiteral("trusting"), QStringLiteral("connecting"), QStringLiteral("done") }));
    }

    // The user cancels the adopted Add: the device's request is refused.
    void cancelAnAdoptedAdd()
    {
        deviceAsks(headset, QStringLiteral("AuthorizeService"), { a2dpSink });
        const Asked question = nextQuestion(0);
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(headset), false });
        nextQuestion(1);
        QCOMPARE(errorOf(call(QStringLiteral("Cancel"), { object(headset) })), QString());
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Cancelled"));
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("answered"));
        QTRY_VERIFY(agentLog().contains(QStringLiteral("AuthorizeService ") + headset + QStringLiteral(" -> org.bluez.Error.Canceled")));
        QVERIFY(!fakeCalls().contains(QStringLiteral("CancelPairing ") + headset));
    }

    // The device gives up on the adopted Add.
    void adoptedAddCancelledByTheDevice()
    {
        deviceAsks(mouse, QStringLiteral("RequestConfirmation"), { 99u });
        nextQuestion(0);
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        nextQuestion(1);
        fake->run([this] { fake->cancelRequestOn(mouse); });
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(add.error().message(), QStringLiteral("Pairing was cancelled on the device."));
    }

    // ---------------------------------------------- who draws (§7.2) --

    // Settings closes while its question is open: the shell draws it.
    void drawnByMovesToTheShell()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.method = FakeBluez::Device::Confirm; });
        // No copy of the connection outlives disconnectFromBus: it closes.
        QDBusConnection::connectToBus(sessionAddress, QStringLiteral("settings"))
            .asyncCall(method(QStringLiteral("Add"), { object(mouse), true }), 120000);
        const Asked question = nextQuestion(0);
        QCOMPARE(question.details.value(QStringLiteral("drawnBy")).toString(), QStringLiteral("settings"));
        QDBusConnection::disconnectFromBus(QStringLiteral("settings"));
        const Asked again = nextQuestion(1);
        QCOMPARE(again.id, question.id);
        QCOMPARE(again.details.value(QStringLiteral("drawnBy")).toString(), QStringLiteral("shell"));
        // The Add goes on without its caller.
        QCOMPARE(answer(question.id, QStringLiteral("accept")), QString());
        QTRY_VERIFY(devices().value(mouse).value(QStringLiteral("connected")).toBool());
    }

    // A client that restarts finds the open question.
    void questionsAfterAClientRestart()
    {
        deviceAsks(mouse, QStringLiteral("RequestAuthorization"));
        const Asked question = nextQuestion(0);
        QDBusConnection restarted = QDBusConnection::connectToBus(sessionAddress, QStringLiteral("restarted"));
        const QList<Asked> open
            = questionsOf(restarted.call(method(QStringLiteral("Questions")), QDBus::Block, 5000));
        QDBusConnection::disconnectFromBus(QStringLiteral("restarted"));
        QCOMPARE(open.size(), 1);
        QCOMPARE(open.first().id, question.id);
        QCOMPARE(open.first().device, mouse);
        QCOMPARE(open.first().kind(), QStringLiteral("authorize-pairing"));
        QCOMPARE(answer(question.id, QStringLiteral("reject")), QString());
    }

    // ----------------------------------------- other sessions (§6.7) --

    // Another desktop's agent is the default while Vela is in the
    // background, and again when Vela goes there.
    void otherSessionsAgent()
    {
        setSession(false, false);
        QTRY_VERIFY(!property(QStringLiteral("Active")).toBool());
        QTRY_VERIFY(defaultAgent().isEmpty());
        QVERIFY(fakeCalls().contains(QStringLiteral("UnregisterAgent")));
        QDBusConnection plasma = QDBusConnection::connectToBus(systemAddress, QStringLiteral("plasma"));
        for (const QString& member : { QStringLiteral("RegisterAgent"), QStringLiteral("RequestDefaultAgent") }) {
            QDBusMessage call = QDBusMessage::createMethodCall(
                QStringLiteral("org.bluez"), QStringLiteral("/org/bluez"), QStringLiteral("org.bluez.AgentManager1"), member);
            call << QVariant::fromValue(QDBusObjectPath(QStringLiteral("/modules/bluedevil/Agent")));
            if (member == QLatin1String("RegisterAgent")) {
                call << QStringLiteral("DisplayYesNo");
            }
            QCOMPARE(errorOf(plasma.call(call, QDBus::Block, 5000)), QString());
        }
        const QString plasmaAgent = plasma.baseService() + QStringLiteral(" /modules/bluedevil/Agent");
        QCOMPARE(defaultAgent(), plasmaAgent);
        // Nothing changes while in the background.
        QCOMPARE(errorOf(start(QStringLiteral("Add"), { object(mouse), false })), QStringLiteral("org.vela.Bluetooth.Error.Inactive"));
        QCOMPARE(errorOf(start(QStringLiteral("SetPowered"), { false })), QStringLiteral("org.vela.Bluetooth.Error.Inactive"));
        QCOMPARE(errorOf(start(QStringLiteral("Rename"), { object(headset), QStringLiteral("x") })),
            QStringLiteral("org.vela.Bluetooth.Error.Inactive"));
        QCOMPARE(answer(1, QStringLiteral("accept")), QStringLiteral("org.vela.Bluetooth.Error.Inactive"));
        QCOMPARE(devices().size(), 3);
        QVERIFY(!fakeCalls().contains(QStringLiteral("Pair ") + mouse));
        // Back in front: Vela's agent is the default, Plasma's under it.
        setSession(true, false);
        QTRY_VERIFY(velaIsTheDefaultAgent());
        QVERIFY(property(QStringLiteral("Active")).toBool());
        setSession(false, false);
        QTRY_COMPARE(defaultAgent(), plasmaAgent);
        setSession(true, false);
        QTRY_VERIFY(velaIsTheDefaultAgent());
        QDBusConnection::disconnectFromBus(QStringLiteral("plasma"));
    }

    void backgroundStopsAnAdd()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.method = FakeBluez::Device::Confirm; });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        const Asked question = nextQuestion(0);
        setSession(false, false);
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(add.error().message(), QStringLiteral("Stopped because you switched to another session."));
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("inactive"));
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("CancelPairing ") + mouse));
        QTRY_VERIFY(agentLog().contains(QStringLiteral("RequestConfirmation ") + mouse + QStringLiteral(" -> org.bluez.Error.Canceled"))
            || agentLog().contains(QStringLiteral("RequestConfirmation ") + mouse + QStringLiteral(" -> cancelled")));
        setSession(true, false);
        QTRY_VERIFY(velaIsTheDefaultAgent());
    }

    // Locked: the agent stays, an Add stops, requests nobody started are
    // refused.
    void lockStopsAnAddAndRefusesRequests()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.method = FakeBluez::Device::Confirm; });
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        const Asked question = nextQuestion(0);
        setSession(true, true);
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        QCOMPARE(add.error().message(), QStringLiteral("Stopped because the screen locked."));
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("locked"));
        QTRY_VERIFY(!property(QStringLiteral("Active")).toBool());
        QVERIFY(velaIsTheDefaultAgent());
        QVERIFY(!fakeCalls().contains(QStringLiteral("UnregisterAgent")));
        deviceAsks(headset, QStringLiteral("AuthorizeService"), { a2dpSink });
        QTRY_VERIFY(agentLog().contains(QStringLiteral("AuthorizeService ") + headset + QStringLiteral(" -> org.bluez.Error.Rejected")));
        QCOMPARE(asked.size(), 1);
        QCOMPARE(errorOf(start(QStringLiteral("Add"), { object(keyboard), false })), QStringLiteral("org.vela.Bluetooth.Error.Inactive"));
        setSession(true, false);
        QTRY_VERIFY(property(QStringLiteral("Active")).toBool());
    }

    // ----------------------------------------------- search leases (§6.6) --

    // Alone, a discoverable lease makes the adapter visible through the
    // filter; stopping it ends Vela's discovery and that visibility.
    void aDiscoverableSearch()
    {
        QCOMPARE(property(QStringLiteral("Search")).toString(), QString());
        const qulonglong lease = startSearch(true);
        QVERIFY(lease > 0);
        QCOMPARE(property(QStringLiteral("Search")).toString(), QStringLiteral("visible"));
        QTRY_COMPARE(discovering().size(), 1);
        QVERIFY(fakeCalls().indexOf(QStringLiteral("SetDiscoveryFilter /org/bluez/hci0 Discoverable=true"))
            < fakeCalls().indexOf(QStringLiteral("StartDiscovery /org/bluez/hci0")));
        QTRY_VERIFY(adapterInUse().value(QStringLiteral("discoverable")).toBool());
        QTRY_VERIFY(adapterInUse().value(QStringLiteral("discovering")).toBool());
        // A second lease shares the same discovery.
        const qulonglong second = startSearch(false);
        QVERIFY(second > lease);
        QCOMPARE(discovering().size(), 1);
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
        QTRY_COMPARE(searchEndReason(lease), QStringLiteral("stopped"));
        QCOMPARE(property(QStringLiteral("Search")).toString(), QStringLiteral("plain"));
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("SetDiscoveryFilter /org/bluez/hci0 Discoverable=false")));
        QTRY_VERIFY(!discoverable());
        QTRY_COMPARE(discovering().size(), 1);
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { second })), QString());
        QTRY_COMPARE(searchEndReason(second), QStringLiteral("stopped"));
        QTRY_VERIFY(discovering().isEmpty());
        QVERIFY(!discoverable());
        // Stopping it again is nothing.
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
        QCOMPARE(searchesEnded.size(), 2);
        // Nothing outlived it.
        QTest::qWait(2500);
        QVERIFY(!property(QStringLiteral("StillVisible")).toBool());
    }

    // A lease ends when its owner leaves the bus; another client can't end
    // it.
    void aLeaseIsItsOwners()
    {
        const qulonglong lease = startSearch(true, nullptr, QDBusConnection::connectToBus(sessionAddress, QStringLiteral("owner")));
        QVERIFY(lease > 0);
        QTRY_COMPARE(discovering().size(), 1);
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
        QTest::qWait(200);
        QVERIFY(searchEndReason(lease).isEmpty());
        QDBusConnection::disconnectFromBus(QStringLiteral("owner"));
        QTRY_COMPARE(searchEndReason(lease), QStringLiteral("owner-gone"));
        QTRY_VERIFY(discovering().isEmpty());
    }

    void aLeaseHasATimeLimit()
    {
        QCOMPARE(stopDaemon(), 0);
        searchLimitMs = 600;
        QVERIFY(startDaemon());
        QTRY_VERIFY(property(QStringLiteral("Available")).toBool());
        QTRY_VERIFY(property(QStringLiteral("Active")).toBool());
        const qulonglong lease = startSearch(true);
        QTRY_COMPARE(discovering().size(), 1);
        QTRY_COMPARE(searchEndReason(lease), QStringLiteral("time-limit"));
        QTRY_VERIFY(discovering().isEmpty());
        QCOMPARE(property(QStringLiteral("Search")).toString(), QString());
    }

    // The lock, the background and the adapter turning off end every
    // lease; none starts meanwhile.
    void leasesEndWithTheSessionAndTheAdapter()
    {
        qulonglong lease = startSearch(true);
        QTRY_COMPARE(discovering().size(), 1);
        setSession(true, true);
        QTRY_COMPARE(searchEndReason(lease), QStringLiteral("locked"));
        QTRY_VERIFY(discovering().isEmpty());
        QString error;
        QCOMPARE(startSearch(true, &error), 0ULL);
        QCOMPARE(error, QStringLiteral("org.vela.Bluetooth.Error.Inactive"));
        setSession(true, false);
        QTRY_VERIFY(property(QStringLiteral("Active")).toBool());

        lease = startSearch(false);
        QTRY_COMPARE(discovering().size(), 1);
        setSession(false, false);
        QTRY_COMPARE(searchEndReason(lease), QStringLiteral("inactive"));
        QTRY_VERIFY(discovering().isEmpty());
        setSession(true, false);
        QTRY_VERIFY(velaIsTheDefaultAgent());

        lease = startSearch(true);
        QTRY_COMPARE(discovering().size(), 1);
        fake->run([this] { fake->setPowered(hci0, false); });
        QTRY_COMPARE(searchEndReason(lease), QStringLiteral("adapter-off"));
        QCOMPARE(startSearch(true, &error), 0ULL);
        QCOMPARE(error, QStringLiteral("org.vela.Bluetooth.Error.Failed"));
        fake->run([this] { fake->setPowered(hci0, true); });
        QTRY_VERIFY(adapterInUse().value(QStringLiteral("powered")).toBool());
    }

    // Another client already discovering: Vela searches without the
    // filter, and says so (rule 1).
    void noFilterWhenAnotherSearches()
    {
        otherDiscovers(true);
        QTRY_VERIFY(adapterInUse().value(QStringLiteral("discovering")).toBool());
        const qulonglong lease = startSearch(true);
        QCOMPARE(property(QStringLiteral("Search")).toString(), QStringLiteral("shared"));
        QTRY_COMPARE(discovering().size(), 2);
        QVERIFY(fakeCalls().contains(QStringLiteral("SetDiscoveryFilter /org/bluez/hci0 Discoverable=false")));
        QVERIFY(!fakeCalls().contains(QStringLiteral("SetDiscoveryFilter /org/bluez/hci0 Discoverable=true")));
        QVERIFY(!discoverable());
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
        QTRY_COMPARE(discovering().size(), 1);
        otherDiscovers(false);
        QTRY_VERIFY(discovering().isEmpty());
    }

    // Another client starts discovering during Vela's search: BlueZ keeps
    // the adapter visible after Vela stops (rule 3). Only "Stop being
    // visible" ends it.
    void stillVisibleAfterTheSearch()
    {
        const qulonglong lease = startSearch(true);
        QTRY_VERIFY(discoverable());
        otherDiscovers(true);
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
        QTRY_COMPARE(discovering().size(), 1);
        QVERIFY(discoverable());
        QTest::qWait(1000);
        QVERIFY(!property(QStringLiteral("StillVisible")).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(property(QStringLiteral("StillVisible")).toBool(), 3000);
        QVERIFY(!fakeWrites().contains(QStringLiteral("/org/bluez/hci0 Discoverable=false")));

        stoppedBeingVisible = true;
        QCOMPARE(errorOf(call(QStringLiteral("StopBeingVisible"))), QString());
        QVERIFY(fakeWrites().contains(QStringLiteral("/org/bluez/hci0 Discoverable=false")));
        QVERIFY(!discoverable());
        QTRY_VERIFY(!property(QStringLiteral("StillVisible")).toBool());
        otherDiscovers(false);
    }

    // Visible before Vela searched: not Vela's doing, nothing to tell.
    void visibleBeforeIsNotReported()
    {
        fake->run([this] { fake->setDiscoverableOutside(hci0, true); });
        QTRY_VERIFY(adapterInUse().value(QStringLiteral("discoverable")).toBool());
        const qulonglong lease = startSearch(true);
        QTRY_COMPARE(discovering().size(), 1);
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
        QTRY_VERIFY(discovering().isEmpty());
        QTest::qWait(2500);
        QVERIFY(!property(QStringLiteral("StillVisible")).toBool());
        QVERIFY(discoverable());
    }

    // An Add suspends discovery while it pairs, and discovery goes on
    // afterwards.
    void anAddSuspendsTheSearch()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.pairHangs = true; });
        const qulonglong lease = startSearch(true);
        QTRY_COMPARE(discovering().size(), 1);
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        QTRY_VERIFY(discovering().isEmpty());
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("Pair ") + mouse));
        QVERIFY(fakeCalls().indexOf(QStringLiteral("StopDiscovery /org/bluez/hci0"))
            < fakeCalls().indexOf(QStringLiteral("Pair ") + mouse));
        QCOMPARE(errorOf(call(QStringLiteral("Cancel"), { object(mouse) })), QString());
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Cancelled"));
        QTRY_COMPARE(discovering().size(), 1);
        QVERIFY(searchEndReason(lease).isEmpty());
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
    }

    // BlueZ stopping by itself: discovery starts again, three times a
    // minute at most.
    void discoveryStartsAgainThreeTimesAMinute()
    {
        QCOMPARE(stopDaemon(), 0);
        restartWindowMs = 3000;
        QVERIFY(startDaemon());
        const auto starts = [this] { return fakeCalls().count(QStringLiteral("StartDiscovery /org/bluez/hci0")); };
        const qulonglong lease = startSearch(false);
        for (int i = 1; i <= 3; ++i) {
            QTRY_COMPARE(discovering().size(), 1);
            QTRY_VERIFY(adapterInUse().value(QStringLiteral("discovering")).toBool());
            fake->run([this] { fake->stopDiscoveryOutside(hci0); });
            QTRY_COMPARE(starts(), i + 1);
        }
        QTRY_COMPARE(discovering().size(), 1);
        QTRY_VERIFY(adapterInUse().value(QStringLiteral("discovering")).toBool());
        fake->run([this] { fake->stopDiscoveryOutside(hci0); });
        QTest::qWait(300);
        QCOMPARE(starts(), 4);
        QVERIFY(discovering().isEmpty());
        QVERIFY(searchEndReason(lease).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(starts(), 5, 5000);
        QTRY_COMPARE(discovering().size(), 1);
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
        QTRY_COMPARE(searchEndReason(lease), QStringLiteral("stopped"));
    }

    // A refused initial discovery is retried without losing its lease.
    void failedInitialStartIsRetried()
    {
        fake->run([this] { fake->adapters[hci0].startErrors = 1; });
        const qulonglong lease = startSearch(false);
        QTRY_VERIFY(fakeCalls().count(QStringLiteral("StartDiscovery /org/bluez/hci0")) >= 2);
        QTRY_COMPARE(discovering().size(), 1);
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
    }

    // A failed StopDiscovery must not orphan BlueZ's active session.
    void failedStopPreservesTheSession()
    {
        const qulonglong lease = startSearch(false);
        QTRY_COMPARE(discovering().size(), 1);
        fake->run([this] { fake->adapters[hci0].stopErrors = 1; });
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
        QTRY_VERIFY(fakeCalls().count(QStringLiteral("StopDiscovery /org/bluez/hci0")) >= 2);
        QTRY_VERIFY(discovering().isEmpty());
        QCOMPARE(fakeCalls().count(QStringLiteral("StartDiscovery /org/bluez/hci0")), 1);
    }

    void delayedFilterNeverStartsDiscoveryDuringPairing()
    {
        onDevice(mouse, [](FakeBluez::Device& device) { device.pairHangs = true; });
        fake->run([this] { fake->adapters[hci0].filterDelayMs = 500; });
        const qulonglong lease = startSearch(true);
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("SetDiscoveryFilter /org/bluez/hci0 Discoverable=true")));
        QDBusPendingCall add = start(QStringLiteral("Add"), { object(mouse), false });
        QTRY_VERIFY(fakeCalls().contains(QStringLiteral("Pair ") + mouse));
        QTest::qWait(700);
        QCOMPARE(fakeCalls().count(QStringLiteral("StartDiscovery /org/bluez/hci0")), 0);
        QCOMPARE(errorOf(call(QStringLiteral("Cancel"), { object(mouse) })), QString());
        QCOMPARE(errorOf(add), QStringLiteral("org.vela.Bluetooth.Error.Cancelled"));
        QTRY_COMPARE(discovering().size(), 1);
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
    }

    void switchingAdapterInvalidatesTheVisibilityCheck()
    {
        const qulonglong lease = startSearch(true);
        QTRY_VERIFY(discoverable());
        otherDiscovers(true);
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
        fake->run([this] {
            fake->addAdapter(hci1, { QStringLiteral("BB:BB:BB:BB:BB:01"), QStringLiteral("dongle") });
            fake->setDiscoverableOutside(hci1, true);
        });
        QTRY_COMPARE(qdbus_cast<QList<QVariantMap>>(property(QStringLiteral("Adapters"))).size(), 2);
        QCOMPARE(errorOf(call(QStringLiteral("UseAdapter"), { QStringLiteral("BB:BB:BB:BB:BB:01") })), QString());
        QTest::qWait(2500);
        QVERIFY(!property(QStringLiteral("StillVisible")).toBool());
        otherDiscovers(false);
    }

    void switchingAdapterStartsNewSearchOnNewAdapter()
    {
        const qulonglong oldLease = startSearch(false);
        QTRY_COMPARE(discovering().size(), 1);
        fake->run([this] { fake->addAdapter(hci1, { QStringLiteral("BB:BB:BB:BB:BB:01"), QStringLiteral("dongle") }); });
        QTRY_COMPARE(qdbus_cast<QList<QVariantMap>>(property(QStringLiteral("Adapters"))).size(), 2);
        QCOMPARE(errorOf(call(QStringLiteral("UseAdapter"), { QStringLiteral("BB:BB:BB:BB:BB:01") })), QString());
        QTRY_COMPARE(searchEndReason(oldLease), QStringLiteral("adapter-off"));
        const qulonglong lease = startSearch(false);
        QTRY_VERIFY([&] {
            bool searching = false;
            fake->run([&] { searching = !fake->adapters[hci1].discovering.isEmpty(); });
            return searching;
        }());
        QTRY_VERIFY(discovering().isEmpty());
        QCOMPARE(errorOf(call(QStringLiteral("StopSearch"), { lease })), QString());
    }

    // ------------------------------------------------ who may answer (§7.3) --

    void otherAppsCannotSeeOrAnswerQuestions()
    {
        deviceAsks(mouse, QStringLiteral("RequestConfirmation"), { 99u });
        const Asked question = nextQuestion(0);
        auto send = [this](const QStringList& arguments) {
            QProcess process;
            process.start(QStringLiteral("dbus-send"),
                QStringList { QStringLiteral("--bus=") + sessionAddress, QStringLiteral("--print-reply"),
                    QStringLiteral("--dest=org.vela.Bluetooth"), objectPath }
                    + arguments);
            process.waitForFinished(10000);
            return QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
        };
        const QString list = send({ QStringLiteral("org.vela.Bluetooth.Questions") });
        QVERIFY2(list.contains(QLatin1String("org.vela.Bluetooth.Error.NotAllowed")), qPrintable(list));
        const QString reply = send({ QStringLiteral("org.vela.Bluetooth.Answer"), QStringLiteral("uint64:%1").arg(question.id),
            QStringLiteral("string:accept"), QStringLiteral("string:") });
        QVERIFY2(reply.contains(QLatin1String("org.vela.Bluetooth.Error.NotAllowed")), qPrintable(reply));
        QCOMPARE(openQuestions().size(), 1);
        QCOMPARE(answer(question.id, QStringLiteral("reject")), QString());
    }

    // The signals go to Vela's UI only, never broadcast: a client that
    // never asked for them gets none.
    void questionSignalsAreNotBroadcast()
    {
        QDBusConnection observer = QDBusConnection::connectToBus(sessionAddress, QStringLiteral("observer"));
        QSignalSpy seen(this, &BluetoothDaemon::observed);
        QVERIFY(observer.connect(service, objectPath, service, QStringLiteral("Question"), this, SIGNAL(observed())));
        QVERIFY(observer.connect(service, objectPath, service, QStringLiteral("QuestionClosed"), this, SIGNAL(observed())));
        deviceAsks(mouse, QStringLiteral("RequestConfirmation"), { 99u });
        const Asked question = nextQuestion(0);
        QCOMPARE(answer(question.id, QStringLiteral("reject")), QString());
        QTRY_COMPARE(closedReason(question.id), QStringLiteral("answered"));
        QTest::qWait(200);
        QCOMPARE(seen.count(), 0);
        QDBusConnection::disconnectFromBus(QStringLiteral("observer"));
    }

    // Only BlueZ talks to the agent.
    void agentAnswersOnlyBluez()
    {
        const QString agent = defaultAgent();
        QDBusConnection intruder = QDBusConnection::connectToBus(systemAddress, QStringLiteral("intruder"));
        QDBusMessage request = QDBusMessage::createMethodCall(
            agent.section(u' ', 0, 0), velaAgent, QStringLiteral("org.bluez.Agent1"), QStringLiteral("RequestAuthorization"));
        request << QVariant::fromValue(QDBusObjectPath(mouse));
        QCOMPARE(errorOf(intruder.call(request, QDBus::Block, 5000)), QStringLiteral("org.bluez.Error.Rejected"));
        QDBusMessage release = QDBusMessage::createMethodCall(
            agent.section(u' ', 0, 0), velaAgent, QStringLiteral("org.bluez.Agent1"), QStringLiteral("Release"));
        QCOMPARE(errorOf(intruder.call(release, QDBus::Block, 5000)), QStringLiteral("org.bluez.Error.Rejected"));
        QDBusConnection::disconnectFromBus(QStringLiteral("intruder"));
        QTest::qWait(100);
        QVERIFY(asked.isEmpty());
    }

signals:
    void devicesChanged();
    void observed();
};

QTEST_GUILESS_MAIN(BluetoothDaemon)
#include "daemon.moc"
