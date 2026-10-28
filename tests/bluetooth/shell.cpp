// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The shell's side of Bluetooth (docs/bluetooth.md §9.2, §9.3, §11) against
// the real vela-bluetooth on a fake BlueZ: the agent's questions the shell
// draws (bluetoothprompts.h), as the dialog or as a notification with its
// actions, closed wherever they were answered; the notification that the PC
// is still visible after a search (§6.6, rule 3); and airplane mode
// (SystemStatus) turning Bluetooth off and on. This test is a UI the test
// build of the daemon trusts, as vela-shell is.

#include "bluetooth/bluetooth.h"
#include "bluetoothprompts.h"
#include "harness.h"
#include "notifications.h"
#include "systemstatus.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <memory>

using vela::bluetooth::Bluetooth;

namespace {

const QString service = QStringLiteral("org.vela.Bluetooth");
const QString hci0 = QStringLiteral("hci0");

QStringList actionKeys(const QVariantList& actions)
{
    QStringList keys;
    for (const QVariant& action : actions) {
        keys << action.toMap().value(QStringLiteral("key")).toString();
    }
    return keys;
}

} // namespace

class BluetoothShell : public QObject, protected BluetoothBench {
    Q_OBJECT

public:
    BluetoothShell()
        : BluetoothBench(QStringLiteral("shell"))
    {
    }

private:
    QString mouse;
    QString phone;

    // The shell: its client of the daemon, the notifications and the
    // questions between them.
    std::unique_ptr<Bluetooth> bluetooth;
    std::unique_ptr<NotificationServer> notifications;
    std::unique_ptr<BluetoothPrompts> prompts;
    bool settingsOpen = false;

    // hci0 on, a mouse and a phone nearby.
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
            device.address = QStringLiteral("00:11:22:33:44:05");
            device.alias = QStringLiteral("Phone <b>");
            device.icon = QStringLiteral("phone");
            phone = fake->addDevice(device);
            fake->calls.clear();
            fake->writes.clear();
            fake->agentLog.clear();
        });
    }

    // The device asks by itself, through the default agent: Vela's, once
    // the shell's client presents questions.
    void deviceAsks(const QString& path, const QString& method, const QVariantList& extra = {})
    {
        QTRY_VERIFY(defaultAgent().contains(QStringLiteral("/org/vela/")));
        fake->run([&] { fake->ask(path, method, extra); });
    }

    QString defaultAgent() const
    {
        QString agent;
        fake->run([&] { agent = fake->defaultAgent(); });
        return agent;
    }

    bool adapterPowered() const
    {
        bool powered = false;
        fake->run([&] { powered = fake->adapters.value(hci0).powered; });
        return powered;
    }

    // The client has read the daemon, and its PresentQuestions() went
    // before this call on the same connection: once this is answered, the
    // daemon has the shell as a presenter, and a device asking by itself is
    // not refused.
    void waitUntilPresenting()
    {
        QTRY_COMPARE(bluetooth->devices().size(), 2);
        QDBusMessage get = QDBusMessage::createMethodCall(service, QStringLiteral("/org/vela/Bluetooth"),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        get << service << QStringLiteral("Active");
        QCOMPARE(bus().call(get).type(), QDBusMessage::ReplyMessage);
    }

    QStringList agentLog() const
    {
        QStringList log;
        fake->run([&] { log = fake->agentLog; });
        return log;
    }

    bool paired(const QString& path) const
    {
        bool result = false;
        fake->run([&] { result = fake->devices.value(path).paired; });
        return result;
    }

    QVariant notificationData(int role) const { return notifications->data(notifications->index(0), role); }

private slots:
    void initTestCase()
    {
        const QString error = setUp();
        QVERIFY2(error.isEmpty(), qPrintable(error));
        // SystemStatus reads NetworkManager and UPower there: none of them,
        // so no Wi-Fi.
        qputenv("DBUS_SYSTEM_BUS_ADDRESS", systemAddress.toUtf8());
    }

    void cleanupTestCase() { tearDown(); }

    void init()
    {
        sessionInFront();
        resetFake();
        QVERIFY(startDaemon());
        bluetooth = std::make_unique<Bluetooth>(bus(), true);
        notifications = std::make_unique<NotificationServer>();
        settingsOpen = false;
        prompts = std::make_unique<BluetoothPrompts>(bluetooth.get(), notifications.get(), [this] { return settingsOpen; });
        waitUntilPresenting();
    }

    void cleanup()
    {
        prompts.reset();
        notifications.reset();
        bluetooth.reset();
        stopDaemon();
    }

    // A device asking to pair by itself is a notification, with the code
    // and Pair or Deny; the name is the device's, escaped in the markup.
    // Pair answers, and the notification is gone.
    void aPhoneAskingIsANotification()
    {
        deviceAsks(phone, QStringLiteral("RequestConfirmation"), { 123456u });
        QTRY_COMPARE(notifications->count(), 1);
        QVERIFY(prompts->dialog().isEmpty());
        QCOMPARE(notificationData(NotificationServer::SummaryRole).toString(), QStringLiteral("Pair with Phone <b>?"));
        const QString body = notificationData(NotificationServer::BodyRole).toString();
        QVERIFY2(body.contains(QLatin1String("123456")) && body.contains(QLatin1String("Phone &lt;b&gt;")), qPrintable(body));
        QCOMPARE(actionKeys(notificationData(NotificationServer::ActionsRole).toList()),
            (QStringList { QStringLiteral("pair"), QStringLiteral("deny") }));
        QVERIFY(notificationData(NotificationServer::CriticalRole).toBool());
        QVERIFY(notificationData(NotificationServer::HasDefaultActionRole).toBool());

        notifications->invoke(prompts->notification(), QStringLiteral("pair"));
        QTRY_VERIFY(agentLog().contains(QStringLiteral("RequestConfirmation ") + phone + QStringLiteral(" -> ok")));
        QTRY_VERIFY(bluetooth->question().isEmpty());
        QCOMPARE(notifications->count(), 0);
        QTRY_VERIFY(paired(phone));
        QVERIFY(bluetooth->error().isEmpty());
    }

    // Deny refuses; so does closing the notification without an answer.
    void denyingOrClosingRefuses()
    {
        deviceAsks(phone, QStringLiteral("RequestAuthorization"));
        QTRY_COMPARE(notifications->count(), 1);
        QCOMPARE(notificationData(NotificationServer::BodyRole).toString(), QStringLiteral("Phone &lt;b&gt; wants to pair with this PC."));
        notifications->invoke(prompts->notification(), QStringLiteral("deny"));
        QTRY_VERIFY(agentLog().contains(QStringLiteral("RequestAuthorization ") + phone + QStringLiteral(" -> org.bluez.Error.Rejected")));
        QTRY_VERIFY(bluetooth->question().isEmpty());

        deviceAsks(phone, QStringLiteral("AuthorizeService"), { QStringLiteral("0000110b-0000-1000-8000-00805f9b34fb") });
        QTRY_COMPARE(notifications->count(), 1);
        QCOMPARE(actionKeys(notificationData(NotificationServer::ActionsRole).toList()),
            (QStringList { QStringLiteral("allow"), QStringLiteral("deny") }));
        notifications->dismiss(prompts->notification());
        QTRY_VERIFY(agentLog().contains(QStringLiteral("AuthorizeService ") + phone + QStringLiteral(" -> org.bluez.Error.Rejected")));
        QTRY_VERIFY(bluetooth->question().isEmpty());
        QVERIFY(!paired(phone));
    }

    // A code to type: "Enter code" opens the dialog with the question,
    // and the dialog's answer goes to the device.
    void enterCodeOpensTheDialog()
    {
        deviceAsks(phone, QStringLiteral("RequestPasskey"));
        QTRY_COMPARE(notifications->count(), 1);
        QCOMPARE(actionKeys(notificationData(NotificationServer::ActionsRole).toList()),
            (QStringList { QStringLiteral("enter"), QStringLiteral("deny") }));
        QSignalSpy dialog(prompts.get(), &BluetoothPrompts::dialogChanged);
        notifications->invoke(prompts->notification(), QStringLiteral("enter"));
        QCOMPARE(dialog.size(), 1);
        QCOMPARE(prompts->dialog().value(QStringLiteral("kind")).toString(), QStringLiteral("enter-passkey"));
        QCOMPARE(notifications->count(), 0);
        // Its notification closing isn't a refusal.
        QTest::qWait(100);
        QVERIFY(!bluetooth->question().isEmpty());

        prompts->answer(QStringLiteral("accept"), QStringLiteral("012345"));
        QTRY_VERIFY(agentLog().contains(QStringLiteral("RequestPasskey ") + phone + QStringLiteral(" -> 12345")));
        QTRY_VERIFY(prompts->dialog().isEmpty());
    }

    // A click on the notification opens the whole question too; a code
    // shown here can only be cancelled.
    void aClickOpensTheDialog()
    {
        deviceAsks(phone, QStringLiteral("DisplayPasskey"), { 123u, QVariant::fromValue(quint16(0)) });
        QTRY_COMPARE(notifications->count(), 1);
        QVERIFY(notificationData(NotificationServer::BodyRole).toString().contains(QLatin1String("000123")));
        QCOMPARE(actionKeys(notificationData(NotificationServer::ActionsRole).toList()), QStringList { QStringLiteral("deny") });
        notifications->invoke(prompts->notification(), QStringLiteral("default"));
        QCOMPARE(prompts->dialog().value(QStringLiteral("kind")).toString(), QStringLiteral("show-passkey"));
        QCOMPARE(prompts->dialog().value(QStringLiteral("passkey")).toString(), QStringLiteral("000123"));
        QSignalSpy closed(bluetooth.get(), &Bluetooth::questionClosed);
        prompts->answer(QStringLiteral("cancel"));
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(closed.at(0).at(1).toString(), QStringLiteral("answered"));
        QVERIFY(prompts->dialog().isEmpty());
    }

    // BlueZ giving up closes the notification, and answers nothing.
    void closedElsewhereClosesTheNotification()
    {
        deviceAsks(phone, QStringLiteral("RequestConfirmation"), { 42u });
        QTRY_COMPARE(notifications->count(), 1);
        fake->run([this] { fake->cancelRequestOn(phone); });
        QTRY_COMPARE(notifications->count(), 0);
        QTRY_VERIFY(bluetooth->question().isEmpty());
        QTest::qWait(100);
        QVERIFY(bluetooth->error().isEmpty());
        QVERIFY(prompts->dialog().isEmpty());
    }

    // The daemon restarting forgets its presenters: the shell is one again
    // once it is back, and its open notification went with the old daemon.
    void presentingAgainAfterARestart()
    {
        deviceAsks(phone, QStringLiteral("RequestAuthorization"));
        QTRY_COMPARE(notifications->count(), 1);
        stopDaemon();
        QTRY_COMPARE(notifications->count(), 0);
        fake->run([this] { fake->forgetAgents(); });
        QTRY_VERIFY(bluetooth->devices().isEmpty());
        QVERIFY(startDaemon());
        waitUntilPresenting();
        deviceAsks(phone, QStringLiteral("RequestAuthorization"));
        QTRY_COMPARE(notifications->count(), 1);
        notifications->invoke(prompts->notification(), QStringLiteral("pair"));
        QTRY_VERIFY(paired(phone));
    }

    // Do not disturb doesn't hide a device that is waiting.
    void doNotDisturbStillShowsIt()
    {
        notifications->setDoNotDisturb(true);
        deviceAsks(phone, QStringLiteral("RequestAuthorization"));
        QTRY_COMPARE(notifications->count(), 1);
        notifications->invoke(prompts->notification(), QStringLiteral("pair"));
        QTRY_VERIFY(paired(phone));
    }

    // An unsolicited question adopted by Settings must close the shell
    // notification without rejecting the Settings-owned Add.
    void settingsAdoptsAnUnsolicitedQuestion()
    {
        deviceAsks(phone, QStringLiteral("RequestConfirmation"), { 123456u });
        QTRY_COMPARE(notifications->count(), 1);
        QVERIFY(QDBusConnection::connectToBus(sessionAddress, QStringLiteral("settings")).isConnected());
        auto settings = std::make_unique<Bluetooth>(QDBusConnection(QStringLiteral("settings")), true);
        QTRY_COMPARE(settings->devices().size(), 2);
        settings->add(phone);
        QTRY_COMPARE(settings->question().value(QStringLiteral("drawnBy")).toString(), QStringLiteral("settings"));
        QTRY_COMPARE(notifications->count(), 0);
        QVERIFY(prompts->dialog().isEmpty());
        settings->answer(settings->question().value(QStringLiteral("id")).toString(), QStringLiteral("accept"));
        QTRY_VERIFY_WITH_TIMEOUT(paired(phone), 10000);
        settings.reset();
        QDBusConnection::disconnectFromBus(QStringLiteral("settings"));
    }

    // An Add without a UI of its own (another app's): the dialog at once,
    // no notification; the answer pairs it.
    void anAddWithoutAUiOpensTheDialog()
    {
        QVERIFY(QDBusConnection::connectToBus(sessionAddress, QStringLiteral("other")).isConnected());
        {
            Bluetooth other(QDBusConnection(QStringLiteral("other")), false);
            QTRY_COMPARE(other.devices().size(), 2);
            onDevice(mouse, FakeBluez::Device::Confirm);
            other.add(mouse);
            QTRY_COMPARE(prompts->dialog().value(QStringLiteral("kind")).toString(), QStringLiteral("confirm-passkey"));
            QCOMPARE(prompts->dialog().value(QStringLiteral("passkey")).toString(), QStringLiteral("123456"));
            QCOMPARE(notifications->count(), 0);
            prompts->answer(QStringLiteral("accept"));
            QTRY_VERIFY(prompts->dialog().isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(paired(mouse), 10000);
        }
        QDBusConnection::disconnectFromBus(QStringLiteral("other"));
    }

    // Settings draws its own Add's question; if it leaves, the shell's
    // dialog takes the same question over.
    void settingsLeavingHandsTheQuestionOver()
    {
        QVERIFY(QDBusConnection::connectToBus(sessionAddress, QStringLiteral("settings")).isConnected());
        auto settings = std::make_unique<Bluetooth>(QDBusConnection(QStringLiteral("settings")), true);
        QTRY_COMPARE(settings->devices().size(), 2);
        onDevice(mouse, FakeBluez::Device::EnterPasskey);
        settings->add(mouse);
        QTRY_COMPARE(settings->question().value(QStringLiteral("drawnBy")).toString(), QStringLiteral("settings"));
        const QString id = settings->question().value(QStringLiteral("id")).toString();
        QTRY_COMPARE(bluetooth->question().value(QStringLiteral("id")).toString(), id);
        QTest::qWait(100);
        QVERIFY(prompts->dialog().isEmpty());
        QCOMPARE(notifications->count(), 0);

        settings.reset();
        QDBusConnection::disconnectFromBus(QStringLiteral("settings"));
        QTRY_COMPARE(prompts->dialog().value(QStringLiteral("id")).toString(), id);
        QCOMPARE(prompts->dialog().value(QStringLiteral("drawnBy")).toString(), QStringLiteral("shell"));
        prompts->answer(QStringLiteral("cancel"));
        QTRY_VERIFY(prompts->dialog().isEmpty());
        QVERIFY(!paired(mouse));
    }

    // Another app keeps the PC visible after Vela's search (Settings', on
    // its own connection): a notification with "Stop being visible", unless
    // Settings is open; it goes when the PC stops being visible.
    void stillVisibleIsANotification()
    {
        QDBusConnection other = QDBusConnection::connectToBus(systemAddress, QStringLiteral("other-searcher"));
        auto discover = [&](bool on) {
            QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), QStringLiteral("/org/bluez/hci0"),
                QStringLiteral("org.bluez.Adapter1"), on ? QStringLiteral("StartDiscovery") : QStringLiteral("StopDiscovery"));
            return other.call(call, QDBus::Block, 5000).type() == QDBusMessage::ReplyMessage;
        };
        QVERIFY(QDBusConnection::connectToBus(sessionAddress, QStringLiteral("settings")).isConnected());
        auto settings = std::make_unique<Bluetooth>(QDBusConnection(QStringLiteral("settings")), true);
        QTRY_COMPARE(settings->devices().size(), 2);
        // Vela's search begins knowing whether another app discovers: it
        // must have seen the other app stop before searching again.
        auto searchAndLeaveVisible = [&] {
            QTRY_VERIFY(!bluetooth->adapter().value(QStringLiteral("discovering")).toBool());
            settings->startSearch();
            QTRY_VERIFY(bluetooth->adapter().value(QStringLiteral("discoverable")).toBool());
            QVERIFY(discover(true));
            settings->stopSearch();
            QTRY_VERIFY_WITH_TIMEOUT(bluetooth->stillVisible(), 4000);
        };

        // Settings open: its page says it.
        settingsOpen = true;
        searchAndLeaveVisible();
        QTest::qWait(100);
        QCOMPARE(notifications->count(), 0);
        settingsOpen = false;
        prompts->settingsWindowsChanged();
        QTRY_COMPARE(notifications->count(), 1);
        settings->stopBeingVisible();
        QTRY_VERIFY(!bluetooth->stillVisible());
        QVERIFY(discover(false));

        settingsOpen = false;
        searchAndLeaveVisible();
        QTRY_COMPARE(notifications->count(), 1);
        QCOMPARE(notificationData(NotificationServer::SummaryRole).toString(), QStringLiteral("Your PC is still visible to nearby devices"));
        QCOMPARE(actionKeys(notificationData(NotificationServer::ActionsRole).toList()), QStringList { QStringLiteral("stop") });
        notifications->invoke(prompts->visibleNotification(), QStringLiteral("stop"));
        QTRY_VERIFY(!bluetooth->stillVisible());
        QTRY_VERIFY(!bluetooth->adapter().value(QStringLiteral("discoverable")).toBool());
        QVERIFY(discover(false));

        // Not visible anymore by someone else's doing: the notification
        // goes.
        searchAndLeaveVisible();
        QTRY_COMPARE(notifications->count(), 1);
        QVERIFY(discover(false));
        QTRY_VERIFY(!bluetooth->stillVisible());
        QTRY_COMPARE(notifications->count(), 0);

        settings.reset();
        QDBusConnection::disconnectFromBus(QStringLiteral("settings"));
        other = QDBusConnection(QString());
        QDBusConnection::disconnectFromBus(QStringLiteral("other-searcher"));
    }

    void airplaneModeDoesNotAssumeTheRadioIsOffWhenDaemonStops()
    {
        SystemStatus status(bluetooth.get());
        QTRY_VERIFY(bluetooth->powered());
        QVERIFY(!status.airplaneMode());
        stopDaemon();
        QTRY_VERIFY(!bluetooth->available());
        QVERIFY(!status.airplaneMode()); // BlueZ still has the powered adapter
        QVERIFY(startDaemon());
        QTRY_VERIFY(bluetooth->powered());
        QVERIFY(!status.airplaneMode());
    }

    // Airplane mode turns Bluetooth off at once, and back on; without
    // Wi-Fi, it is Bluetooth alone.
    void airplaneModeTurnsBluetoothOff()
    {
        SystemStatus status(bluetooth.get());
        QTRY_VERIFY(bluetooth->powered());
        QVERIFY(!status.airplaneMode());
        QSignalSpy airplane(&status, &SystemStatus::airplaneChanged);
        status.setAirplaneMode(true);
        QVERIFY(status.airplaneMode());
        QVERIFY(!airplane.isEmpty());
        QTRY_VERIFY(!adapterPowered());
        QVERIFY(status.airplaneMode());
        status.setAirplaneMode(false);
        QVERIFY(!status.airplaneMode());
        QTRY_VERIFY(adapterPowered());
        QTRY_VERIFY(bluetooth->powered());
    }

private:
    void onDevice(const QString& path, decltype(FakeBluez::Device::method) method)
    {
        fake->run([&] { fake->devices[path].method = method; });
    }
};

QTEST_GUILESS_MAIN(BluetoothShell)
#include "shell.moc"
