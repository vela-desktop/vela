// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The battery notifications end to end (docs/battery-notifications.md §9):
// a fake UPower on a private bus, the real UPowerBatteries, BatteryAlerts
// and BatteryNotices, and the notifications as NotificationServer shows
// them (popups, the notification center, Do not disturb).

#include "batterynotices.h"
#include "notifications.h"
#include "upowerbatteries.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVirtualObject>
#include <QGuiApplication>
#include <QProcess>
#include <QTest>
#include <QTimer>

#include <memory>

namespace {

const QString service = QStringLiteral("org.freedesktop.UPower");
const QString root = QStringLiteral("/org/freedesktop/UPower");
const QString deviceInterface = QStringLiteral("org.freedesktop.UPower.Device");
const QString propertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");
const QString mousePath = QStringLiteral("/org/freedesktop/UPower/devices/battery_hidpp_battery_0");
const QString headsetPath = QStringLiteral("/org/freedesktop/UPower/devices/headset_dev_7C_E9_13_88_42_BE");
const QString bat0Path = QStringLiteral("/org/freedesktop/UPower/devices/battery_BAT0");

// UPower: EnumerateDevices, each device's GetAll, and the signals the
// tests make it send. It counts the GetAll calls per device.
class FakeUPower : public QDBusVirtualObject {
public:
    QString busName;
    QMap<QString, QVariantMap> devices; // the DisplayDevice too
    QHash<QString, int> reads;
    // The next GetAll of a device answered this late (ms), with the
    // properties it had when asked.
    QHash<QString, int> delayNext;

    QString introspect(const QString&) const override { return {}; }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
    {
        if (message.path() == root && message.member() == QLatin1String("EnumerateDevices")) {
            // Marshalled by hand: the shell must decode "ao" on its own.
            QDBusArgument paths;
            paths.beginArray(QMetaType::fromType<QDBusObjectPath>());
            for (auto it = devices.cbegin(); it != devices.cend(); ++it) {
                if (it.key() != UPowerBatteries::displayDevice) {
                    paths << QDBusObjectPath(it.key());
                }
            }
            paths.endArray();
            connection.send(message.createReply(QVariant::fromValue(paths)));
            return true;
        }
        if (message.interface() == propertiesInterface && message.member() == QLatin1String("GetAll")
            && devices.contains(message.path())) {
            ++reads[message.path()];
            const QDBusMessage reply = message.createReply(QVariant(devices.value(message.path())));
            if (const int delay = delayNext.take(message.path())) {
                message.setDelayedReply(true);
                QTimer::singleShot(delay, [connection, reply] { connection.send(reply); });
            } else {
                connection.send(reply);
            }
            return true;
        }
        connection.send(message.createErrorReply(QDBusError::UnknownMethod, message.member()));
        return true;
    }

    QDBusConnection bus() const { return QDBusConnection(busName); }

    void set(const QString& path, const QVariantMap& changes)
    {
        for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
            devices[path][it.key()] = it.value();
        }
        QDBusMessage signal = QDBusMessage::createSignal(path, propertiesInterface, QStringLiteral("PropertiesChanged"));
        signal << deviceInterface << QVariant(changes) << QStringList();
        bus().send(signal);
    }

    void add(const QString& path, const QVariantMap& properties)
    {
        devices[path] = properties;
        QDBusMessage signal = QDBusMessage::createSignal(root, service, QStringLiteral("DeviceAdded"));
        signal << QVariant::fromValue(QDBusObjectPath(path));
        bus().send(signal);
    }

    void remove(const QString& path)
    {
        devices.remove(path);
        QDBusMessage signal = QDBusMessage::createSignal(root, service, QStringLiteral("DeviceRemoved"));
        signal << QVariant::fromValue(QDBusObjectPath(path));
        bus().send(signal);
    }
};

QVariantMap mouse(uint warning, uint level)
{
    return {
        { QStringLiteral("NativePath"), QStringLiteral("hidpp_battery_0") },
        { QStringLiteral("Vendor"), QStringLiteral("Logitech, Inc.") },
        { QStringLiteral("Model"), QStringLiteral("Logitech MX Vertical") },
        { QStringLiteral("Serial"), QStringLiteral("e6-43-fc-59") },
        { QStringLiteral("Type"), 5u },
        { QStringLiteral("PowerSupply"), false },
        { QStringLiteral("IsPresent"), true },
        { QStringLiteral("State"), 2u },
        { QStringLiteral("BatteryLevel"), level },
        { QStringLiteral("WarningLevel"), warning },
        { QStringLiteral("Percentage"), 50.0 },
    };
}

QVariantMap headset()
{
    return {
        { QStringLiteral("NativePath"), QStringLiteral("/org/bluez/hci0/dev_7C_E9_13_88_42_BE") },
        { QStringLiteral("Serial"), QStringLiteral("7C:E9:13:88:42:BE") },
        { QStringLiteral("Type"), 17u },
        { QStringLiteral("PowerSupply"), false },
        { QStringLiteral("IsPresent"), false },
        { QStringLiteral("State"), 0u },
        { QStringLiteral("WarningLevel"), 1u },
        { QStringLiteral("Percentage"), 100.0 },
    };
}

QVariantMap battery(double percentage, uint warning = 1, uint state = 2)
{
    return {
        { QStringLiteral("NativePath"), QStringLiteral("BAT0") },
        { QStringLiteral("Type"), 2u },
        { QStringLiteral("PowerSupply"), true },
        { QStringLiteral("IsPresent"), true },
        { QStringLiteral("State"), state },
        { QStringLiteral("WarningLevel"), warning },
        { QStringLiteral("Percentage"), percentage },
    };
}

QStringList summaries(const QAbstractItemModel* model)
{
    QStringList out;
    for (int row = 0; row < model->rowCount(); ++row) {
        out << model->index(row, 0).data(NotificationServer::SummaryRole).toString();
    }
    return out;
}

QString body(const QAbstractItemModel* model, const QString& summary)
{
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, 0).data(NotificationServer::SummaryRole).toString() == summary) {
            return model->index(row, 0).data(NotificationServer::BodyRole).toString();
        }
    }
    return {};
}

const QString mouseLow = QStringLiteral("Logitech MX Vertical battery is low");
const QString mouseCritical = QStringLiteral("Logitech MX Vertical battery is critical");
const QString pcLow = QStringLiteral("Battery low");
const QString pcCritical = QStringLiteral("Battery critically low");

} // namespace

class BatteryNoticesTest : public QObject {
    Q_OBJECT

    QProcess m_daemon;
    QString m_address;
    int m_connections = 0;
    std::unique_ptr<FakeUPower> m_fake;

    // UPower on the bus with these devices (a new owner each time: a restart).
    bool startUPower()
    {
        const QString name = QStringLiteral("fake-upower-%1").arg(++m_connections);
        QDBusConnection bus = QDBusConnection::connectToBus(m_address, name);
        m_fake->busName = name;
        return bus.registerVirtualObject(root, m_fake.get(), QDBusConnection::SubPath)
            && bus.registerService(service);
    }

    void stopUPower()
    {
        QDBusConnection::disconnectFromBus(m_fake->busName);
    }

    QDBusConnection client() const { return QDBusConnection(QStringLiteral("shell")); }

private Q_SLOTS:
    void initTestCase()
    {
        m_daemon.start(QStringLiteral("dbus-daemon"), { QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1") });
        if (!m_daemon.waitForStarted() || !m_daemon.waitForReadyRead()) {
            QSKIP("dbus-daemon is needed");
        }
        m_address = QString::fromUtf8(m_daemon.readAllStandardOutput().trimmed());
        QVERIFY(QDBusConnection::connectToBus(m_address, QStringLiteral("shell")).isConnected());
    }

    void init()
    {
        m_fake = std::make_unique<FakeUPower>();
        m_fake->devices[UPowerBatteries::displayDevice] = battery(80);
        m_fake->devices[bat0Path] = battery(80);
        m_fake->devices[mousePath] = mouse(1, 6);
        m_fake->devices[headsetPath] = headset();
        QVERIFY(startUPower());
    }

    void cleanup()
    {
        stopUPower();
        m_fake.reset();
    }

    void cleanupTestCase()
    {
        QDBusConnection::disconnectFromBus(QStringLiteral("shell"));
        m_daemon.terminate();
        m_daemon.waitForFinished();
    }

    void lowAtStartThenCritical()
    {
        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 3u }, { QStringLiteral("BatteryLevel"), 3u } });
        NotificationServer server;
        BatteryNotices notices(&server, client());
        notices.start();
        QTRY_COMPARE(summaries(&server), QStringList { mouseLow });
        QCOMPARE(body(&server, mouseLow), QStringLiteral("Recharge the device soon."));
        QCOMPARE(server.index(0).data(NotificationServer::IconRole).toString().contains(QLatin1String("input-mouse")), true);

        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 4u }, { QStringLiteral("BatteryLevel"), 4u } });
        // The low one goes, critical gets its own popup.
        QTRY_COMPARE(summaries(&server), QStringList { mouseCritical });
        QCOMPARE(server.index(0).data(NotificationServer::CriticalRole).toBool(), true);
        QCOMPARE(summaries(qobject_cast<QAbstractItemModel*>(server.history())), QStringList());
    }

    // Each PropertiesChanged reads only the device that sent it; BAT0, never
    // a battery to tell about, isn't followed after its first reading.
    void onlyTheSenderIsRead()
    {
        NotificationServer server;
        BatteryNotices notices(&server, client());
        notices.start();
        QTRY_VERIFY(m_fake->reads.value(mousePath) == 1 && m_fake->reads.value(headsetPath) == 1
            && m_fake->reads.value(bat0Path) == 1 && m_fake->reads.value(UPowerBatteries::displayDevice) == 1);
        m_fake->set(mousePath, { { QStringLiteral("Percentage"), 45.0 } });
        QTRY_COMPARE(m_fake->reads.value(mousePath), 2);
        m_fake->set(bat0Path, { { QStringLiteral("Percentage"), 79.0 } });
        m_fake->set(headsetPath, { { QStringLiteral("Percentage"), 90.0 } });
        QTRY_COMPARE(m_fake->reads.value(headsetPath), 2);
        QCOMPARE(m_fake->reads.value(mousePath), 2);
        QCOMPARE(m_fake->reads.value(bat0Path), 1);
        QCOMPARE(m_fake->reads.value(UPowerBatteries::displayDevice), 1);
    }

    void bouncingAndReconnecting()
    {
        NotificationServer server;
        BatteryNotices notices(&server, client());
        notices.start();
        QTRY_COMPARE(notices.readings(), 3); // DisplayDevice, mouse, headset
        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 3u }, { QStringLiteral("BatteryLevel"), 3u } });
        QTRY_COMPARE(summaries(&server), QStringList { mouseLow });
        // Normal and low again, while connected: nothing new.
        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 1u }, { QStringLiteral("BatteryLevel"), 6u } });
        QTRY_COMPARE(notices.readings(), 5); // one at a time: a newer request drops an older reply
        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 3u }, { QStringLiteral("BatteryLevel"), 3u } });
        QTRY_COMPARE(notices.readings(), 6);
        QCOMPARE(summaries(&server), QStringList { mouseLow });

        // Away and back on another path (the same serial): not told again.
        const QString again = QStringLiteral("/org/freedesktop/UPower/devices/battery_hidpp_battery_1");
        QVariantMap properties = m_fake->devices.value(mousePath);
        m_fake->remove(mousePath);
        m_fake->add(again, properties);
        QTRY_COMPARE(notices.readings(), 7);
        QCOMPARE(summaries(&server), QStringList { mouseLow });

        // Away and back charged (batteries replaced): the warning goes.
        properties[QStringLiteral("WarningLevel")] = 1u;
        properties[QStringLiteral("BatteryLevel")] = 8u;
        m_fake->remove(again);
        m_fake->add(mousePath, properties);
        QTRY_COMPARE(summaries(&server), QStringList());
        QCOMPARE(summaries(qobject_cast<QAbstractItemModel*>(server.history())), QStringList());
        QVERIFY(notices.notification(QStringLiteral("serial:Logitech, Inc.:e6-43-fc-59")) == 0);
    }

    void upowerRestarts()
    {
        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 3u }, { QStringLiteral("BatteryLevel"), 3u } });
        NotificationServer server;
        BatteryNotices notices(&server, client());
        notices.start();
        QTRY_COMPARE(summaries(&server), QStringList { mouseLow });
        stopUPower();
        QVERIFY(startUPower());
        QTRY_COMPARE(notices.readings(), 6);
        // Still low: not told twice. Still followed: critical is told.
        QCOMPARE(summaries(&server), QStringList { mouseLow });
        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 4u }, { QStringLiteral("BatteryLevel"), 4u } });
        QTRY_COMPARE(summaries(&server), QStringList { mouseCritical });
    }

    void thePCsBattery()
    {
        NotificationServer server;
        BatteryNotices notices(&server, client());
        notices.configure({ true, 25, true });
        notices.start();
        QTRY_COMPARE(notices.readings(), 3);
        m_fake->set(UPowerBatteries::displayDevice, { { QStringLiteral("Percentage"), 26.0 } });
        QTRY_COMPARE(notices.readings(), 4);
        QCOMPARE(summaries(&server), QStringList());
        m_fake->set(UPowerBatteries::displayDevice, { { QStringLiteral("Percentage"), 24.6 } });
        QTRY_COMPARE(summaries(&server), QStringList { pcLow });
        QCOMPARE(body(&server, pcLow), QStringLiteral("25% remaining. Plug in your PC."));
        m_fake->set(UPowerBatteries::displayDevice, { { QStringLiteral("Percentage"), 5.0 }, { QStringLiteral("WarningLevel"), 4u } });
        QTRY_COMPARE(summaries(&server), QStringList { pcCritical });
        QCOMPARE(body(&server, pcCritical), QStringLiteral("5% remaining. Plug in your PC now."));
        // Plugged in: gone, from the popups and the notification center.
        m_fake->set(UPowerBatteries::displayDevice, { { QStringLiteral("State"), 1u }, { QStringLiteral("WarningLevel"), 1u } });
        QTRY_COMPARE(summaries(&server), QStringList());
        QCOMPARE(summaries(qobject_cast<QAbstractItemModel*>(server.history())), QStringList());
    }

    void settingsLive()
    {
        m_fake->devices[UPowerBatteries::displayDevice] = battery(15);
        NotificationServer server;
        BatteryNotices notices(&server, client());
        notices.configure({ false, 20, true });
        notices.start();
        QTRY_COMPARE(notices.readings(), 3);
        QCOMPARE(summaries(&server), QStringList()); // off
        notices.configure({ true, 20, true });
        QCOMPARE(summaries(&server), QStringList { pcLow });
        notices.configure({ true, 10, true }); // under the charge now
        QCOMPARE(summaries(&server), QStringList());
        notices.configure({ true, 15, true });
        QCOMPARE(summaries(&server), QStringList { pcLow });
        notices.configure({ false, 15, true });
        QCOMPARE(summaries(&server), QStringList());
        QCOMPARE(summaries(qobject_cast<QAbstractItemModel*>(server.history())), QStringList());
    }

    // Low ones wait in the notification center; critical ones pop up anyway.
    // A peripheral's critical one expires there, the PC's stays.
    void doNotDisturbAndExpiry()
    {
        NotificationServer server;
        server.setDoNotDisturb(true);
        BatteryNotices notices(&server, client());
        notices.start();
        QTRY_COMPARE(notices.readings(), 3);
        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 3u }, { QStringLiteral("BatteryLevel"), 3u } });
        QTRY_COMPARE(summaries(qobject_cast<QAbstractItemModel*>(server.history())), QStringList { mouseLow });
        QCOMPARE(summaries(&server), QStringList());

        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 4u }, { QStringLiteral("BatteryLevel"), 4u } });
        m_fake->set(UPowerBatteries::displayDevice, { { QStringLiteral("Percentage"), 3.0 }, { QStringLiteral("WarningLevel"), 4u } });
        QTRY_COMPARE(summaries(&server).size(), 2);
        QVERIFY(summaries(&server).contains(mouseCritical) && summaries(&server).contains(pcCritical));
        QCOMPARE(summaries(qobject_cast<QAbstractItemModel*>(server.history())), QStringList()); // the low one went
        QTRY_COMPARE_WITH_TIMEOUT(summaries(&server), QStringList { pcCritical }, 10000);
        QCOMPARE(summaries(qobject_cast<QAbstractItemModel*>(server.history())), QStringList { mouseCritical });
    }

    // Dismissed: not told again until it rearms.
    void dismissed()
    {
        NotificationServer server;
        BatteryNotices notices(&server, client());
        notices.start();
        QTRY_COMPARE(notices.readings(), 3);
        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 3u }, { QStringLiteral("BatteryLevel"), 3u } });
        QTRY_COMPARE(summaries(&server), QStringList { mouseLow });
        server.CloseNotification(server.index(0).data(NotificationServer::IdRole).toUInt());
        QCOMPARE(summaries(&server), QStringList());
        m_fake->set(mousePath, { { QStringLiteral("Percentage"), 40.0 } });
        QTRY_COMPARE(notices.readings(), 5);
        QCOMPARE(summaries(&server), QStringList());
        // Charged and low again: told.
        m_fake->set(mousePath, { { QStringLiteral("State"), 1u } });
        QTRY_COMPARE(notices.readings(), 6);
        m_fake->set(mousePath, { { QStringLiteral("State"), 2u } });
        QTRY_COMPARE(summaries(&server), QStringList { mouseLow });
    }

    // A reply that comes after a newer one is dropped: it tells of a state
    // already gone.
    void staleRepliesAreDropped()
    {
        NotificationServer server;
        BatteryNotices notices(&server, client());
        notices.start();
        QTRY_COMPARE(notices.readings(), 3);
        m_fake->delayNext[mousePath] = 500;
        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 4u }, { QStringLiteral("BatteryLevel"), 4u } });
        QTRY_COMPARE(m_fake->reads.value(mousePath), 2);
        m_fake->set(mousePath, { { QStringLiteral("WarningLevel"), 3u }, { QStringLiteral("BatteryLevel"), 3u } });
        QTRY_COMPARE(summaries(&server), QStringList { mouseLow });
        QTest::qWait(800); // the critical reading arrives, and is dropped
        QCOMPARE(summaries(&server), QStringList { mouseLow });
        QCOMPARE(notices.readings(), 4);
    }

    // A serial seen only later doesn't make it another device.
    void identityStaysWhileConnected()
    {
        NotificationServer server;
        BatteryNotices notices(&server, client());
        notices.start();
        QTRY_COMPARE(notices.readings(), 3);
        const QString keyboardPath = QStringLiteral("/org/freedesktop/UPower/devices/keyboard_dev_1");
        m_fake->add(keyboardPath, { { QStringLiteral("Type"), 6u }, { QStringLiteral("State"), 2u },
            { QStringLiteral("WarningLevel"), 3u }, { QStringLiteral("Percentage"), 9.0 } });
        QTRY_COMPARE(summaries(&server), QStringList { QStringLiteral("Keyboard battery is low") });
        m_fake->set(keyboardPath, { { QStringLiteral("Serial"), QStringLiteral("AA:BB") } });
        QTRY_COMPARE(notices.readings(), 5);
        QCOMPARE(summaries(&server), QStringList { QStringLiteral("Keyboard battery is low") });
    }

    // No UPower at all: nothing, and no error.
    void noUPower()
    {
        stopUPower();
        NotificationServer server;
        BatteryNotices notices(&server, client());
        notices.start();
        QTest::qWait(200);
        QCOMPARE(summaries(&server), QStringList());
        QVERIFY(startUPower()); // for cleanup()
    }
};

QTEST_MAIN(BatteryNoticesTest)
#include "batterynotices.moc"
