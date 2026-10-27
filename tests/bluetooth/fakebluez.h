// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A fake BlueZ for the Bluetooth tests (docs/bluetooth.md §11), on a private
// system bus. It serves the ObjectManager, Adapter1, Device1 and Battery1,
// and each device follows a script: how pairing ends and after how long,
// whether Connect and Set Trusted work. It records the calls it gets and
// every property written, so the tests can check what vela-bluetooth
// changed.
//
// It lives in a thread of its own (the daemon under test calls it while the
// test waits); the test reaches it through run().

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QMap>
#include <QMetaObject>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <functional>

using FakeInterfaces = QMap<QString, QVariantMap>;
using FakeObjects = QMap<QDBusObjectPath, FakeInterfaces>;
Q_DECLARE_METATYPE(FakeInterfaces)
Q_DECLARE_METATYPE(FakeObjects)

class FakeBluez : public QDBusVirtualObject {
public:
    struct Adapter {
        QString address;
        QString alias;
        bool powered = true;
        // How Set Powered goes: done 50 ms later, refused, or accepted and
        // never done.
        enum { Works, Refuses, Never } power = Works;
    };

    struct Device {
        QString adapter; // "hci0"
        QString address;
        QString alias;
        QString icon;
        bool paired = false;
        bool trusted = false;
        bool connected = false;
        int battery = -1;
        int rssi = 0; // 0: none
        // Pair: replied after `pairDelay` ms with `pairError` (empty: it
        // works), or never (`pairHangs`) until CancelPairing.
        QString pairError;
        int pairDelay = 20;
        bool pairHangs = false;
        QString trustError;
        QString connectError;
        QString connectMessage;
        int connectDelay = 20;
    };

    QMap<QString, Adapter> adapters; // by name: "hci0"
    QMap<QString, Device> devices; // by path
    QStringList calls; // "Pair /org/bluez/hci0/dev_..."
    QStringList writes; // "/org/bluez/hci0 Powered=true"
    QString bus;

    static QString adapterPath(const QString& name) { return QStringLiteral("/org/bluez/") + name; }

    static QString devicePath(const QString& adapter, const QString& address)
    {
        return adapterPath(adapter) + QStringLiteral("/dev_") + QString(address).replace(u':', u'_');
    }

    QDBusConnection connection() const { return QDBusConnection(bus); }

    // Runs `work` in the fake's thread and waits for it.
    void run(const std::function<void()>& work) { QMetaObject::invokeMethod(this, work, Qt::BlockingQueuedConnection); }

    // ---------------------------------------------------- the bus (D-Bus) --

    QString introspect(const QString&) const override { return {}; }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection&) override
    {
        const QString path = message.path();
        const QString interface = message.interface();
        const QString member = message.member();
        if (interface == QLatin1String("org.freedesktop.DBus.ObjectManager") && member == QLatin1String("GetManagedObjects")) {
            FakeObjects objects;
            for (auto it = adapters.cbegin(); it != adapters.cend(); ++it) {
                objects[QDBusObjectPath(adapterPath(it.key()))] = adapterInterfaces(it.key());
            }
            for (auto it = devices.cbegin(); it != devices.cend(); ++it) {
                objects[QDBusObjectPath(it.key())] = deviceInterfaces(it.key());
            }
            send(message.createReply(QVariant::fromValue(objects)));
            return true;
        }
        if (interface == QLatin1String("org.freedesktop.DBus.Properties")) {
            return properties(message);
        }
        if (interface == QLatin1String("org.bluez.Adapter1") && member == QLatin1String("RemoveDevice")) {
            const QString device = message.arguments().value(0).value<QDBusObjectPath>().path();
            calls << QStringLiteral("RemoveDevice ") + device;
            if (!devices.contains(device)) {
                send(message.createErrorReply(QStringLiteral("org.bluez.Error.DoesNotExist"), QStringLiteral("Does Not Exist")));
                return true;
            }
            send(message.createReply());
            QTimer::singleShot(20, this, [this, device] { removeDevice(device); });
            return true;
        }
        if (interface == QLatin1String("org.bluez.Device1") && devices.contains(path)) {
            calls << member + u' ' + path;
            if (member == QLatin1String("Pair")) {
                pair(message);
            } else if (member == QLatin1String("CancelPairing")) {
                send(message.createReply());
                if (pending.contains(path)) {
                    send(pending.take(path).createErrorReply(
                        QStringLiteral("org.bluez.Error.AuthenticationCanceled"), QStringLiteral("Authentication Canceled")));
                }
            } else if (member == QLatin1String("Connect")) {
                connectDevice(message);
            } else if (member == QLatin1String("Disconnect")) {
                if (!devices[path].connected) {
                    send(message.createErrorReply(QStringLiteral("org.bluez.Error.NotConnected"), QStringLiteral("Not Connected")));
                } else {
                    setDevice(path, QStringLiteral("Connected"), false);
                    send(message.createReply());
                }
            } else {
                return false;
            }
            return true;
        }
        return false;
    }

    // ------------------------------------------ what the test changes --

    void addAdapter(const QString& name, const Adapter& adapter)
    {
        adapters[name] = adapter;
        interfacesAdded(adapterPath(name), adapterInterfaces(name));
    }

    void removeAdapter(const QString& name)
    {
        for (const QString& path : devices.keys()) {
            if (devices[path].adapter == name) {
                removeDevice(path);
            }
        }
        adapters.remove(name);
        QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
            QStringLiteral("InterfacesRemoved"));
        signal << QVariant::fromValue(QDBusObjectPath(adapterPath(name)))
               << QStringList { QStringLiteral("org.bluez.Adapter1") };
        send(signal);
    }

    QString addDevice(const Device& device)
    {
        const QString path = devicePath(device.adapter, device.address);
        devices[path] = device;
        interfacesAdded(path, deviceInterfaces(path));
        return path;
    }

    void removeDevice(const QString& path)
    {
        if (!devices.contains(path)) {
            return;
        }
        devices.remove(path);
        if (pending.contains(path)) {
            send(pending.take(path).createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.UnknownObject"), QStringLiteral("gone")));
        }
        QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
            QStringLiteral("InterfacesRemoved"));
        signal << QVariant::fromValue(QDBusObjectPath(path))
               << QStringList { QStringLiteral("org.bluez.Device1"), QStringLiteral("org.bluez.Battery1") };
        send(signal);
    }

    // Outside Vela: a hardware key, another desktop.
    void setPowered(const QString& name, bool on)
    {
        adapters[name].powered = on;
        changed(adapterPath(name), QStringLiteral("org.bluez.Adapter1"),
            { { QStringLiteral("Powered"), on }, { QStringLiteral("PowerState"), on ? QStringLiteral("on") : QStringLiteral("off") } });
    }

    void setDevice(const QString& path, const QString& name, const QVariant& value)
    {
        Device& device = devices[path];
        if (name == QLatin1String("Paired")) {
            device.paired = value.toBool();
        } else if (name == QLatin1String("Trusted")) {
            device.trusted = value.toBool();
        } else if (name == QLatin1String("Connected")) {
            device.connected = value.toBool();
        } else if (name == QLatin1String("Alias")) {
            device.alias = value.toString();
        }
        changed(path, QStringLiteral("org.bluez.Device1"), { { name, value } });
    }

    // bluetoothd stops (`leave`) and starts again (`come back`).
    bool leave() { return connection().unregisterService(QStringLiteral("org.bluez")); }
    bool comeBack() { return connection().registerService(QStringLiteral("org.bluez")); }

private:
    QMap<QString, QDBusMessage> pending; // Pair waiting forever, by device

    void send(const QDBusMessage& message) { connection().send(message); }

    FakeInterfaces adapterInterfaces(const QString& name) const
    {
        const Adapter& adapter = adapters[name];
        return { { QStringLiteral("org.bluez.Adapter1"),
            QVariantMap { { QStringLiteral("Address"), adapter.address }, { QStringLiteral("Alias"), adapter.alias },
                { QStringLiteral("Name"), adapter.alias }, { QStringLiteral("Powered"), adapter.powered },
                { QStringLiteral("PowerState"), adapter.powered ? QStringLiteral("on") : QStringLiteral("off") },
                { QStringLiteral("Discoverable"), false }, { QStringLiteral("Discovering"), false },
                { QStringLiteral("Pairable"), true } } } };
    }

    FakeInterfaces deviceInterfaces(const QString& path) const
    {
        const Device& device = devices[path];
        QVariantMap properties { { QStringLiteral("Address"), device.address }, { QStringLiteral("Alias"), device.alias },
            { QStringLiteral("Name"), device.alias }, { QStringLiteral("Icon"), device.icon },
            { QStringLiteral("Class"), quint32(0) }, { QStringLiteral("Paired"), device.paired },
            { QStringLiteral("Bonded"), device.paired }, { QStringLiteral("Trusted"), device.trusted },
            { QStringLiteral("Connected"), device.connected }, { QStringLiteral("Blocked"), false },
            { QStringLiteral("Adapter"), QVariant::fromValue(QDBusObjectPath(adapterPath(device.adapter))) } };
        if (device.rssi) {
            properties[QStringLiteral("RSSI")] = QVariant::fromValue(qint16(device.rssi));
        }
        FakeInterfaces interfaces { { QStringLiteral("org.bluez.Device1"), properties } };
        if (device.battery >= 0) {
            interfaces[QStringLiteral("org.bluez.Battery1")]
                = QVariantMap { { QStringLiteral("Percentage"), QVariant::fromValue(uchar(device.battery)) } };
        }
        return interfaces;
    }

    void interfacesAdded(const QString& path, const FakeInterfaces& interfaces)
    {
        QDBusMessage signal = QDBusMessage::createSignal(
            QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("InterfacesAdded"));
        signal << QVariant::fromValue(QDBusObjectPath(path)) << QVariant::fromValue(interfaces);
        send(signal);
    }

    void changed(const QString& path, const QString& interface, const QVariantMap& values)
    {
        QDBusMessage signal = QDBusMessage::createSignal(path, QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        signal << interface << values << QStringList();
        send(signal);
    }

    bool properties(const QDBusMessage& message)
    {
        const QString path = message.path();
        const QString name = message.arguments().value(1).toString();
        const QString adapter = path.section(u'/', 3, 3);
        if (message.member() == QLatin1String("Get")) {
            QVariant value;
            if (adapters.contains(adapter) && path == adapterPath(adapter) && name == QLatin1String("Powered")) {
                value = adapters[adapter].powered;
            }
            send(message.createReply(QVariant::fromValue(QDBusVariant(value))));
            return true;
        }
        if (message.member() != QLatin1String("Set")) {
            return false;
        }
        const QVariant value = message.arguments().value(2).value<QDBusVariant>().variant();
        writes << QStringLiteral("%1 %2=%3").arg(path, name, value.toString());
        if (adapters.contains(adapter) && path == adapterPath(adapter)) {
            Adapter& target = adapters[adapter];
            if (name == QLatin1String("Powered") && target.power == Adapter::Refuses) {
                send(message.createErrorReply(QStringLiteral("org.bluez.Error.Failed"), QStringLiteral("Failed")));
                return true;
            }
            send(message.createReply());
            if (name == QLatin1String("Powered") && target.power == Adapter::Works) {
                QTimer::singleShot(50, this, [this, adapter, on = value.toBool()] {
                    if (adapters.contains(adapter)) {
                        setPowered(adapter, on);
                    }
                });
            }
            return true;
        }
        if (!devices.contains(path)) {
            send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.UnknownObject"), path));
            return true;
        }
        if (name == QLatin1String("Trusted") && !devices[path].trustError.isEmpty()) {
            send(message.createErrorReply(devices[path].trustError, QStringLiteral("no")));
            return true;
        }
        setDevice(path, name, value);
        send(message.createReply());
        return true;
    }

    void pair(const QDBusMessage& message)
    {
        const QString path = message.path();
        const Device& device = devices[path];
        if (device.paired) {
            send(message.createErrorReply(QStringLiteral("org.bluez.Error.AlreadyExists"), QStringLiteral("Already Exists")));
            return;
        }
        if (device.pairHangs) {
            message.setDelayedReply(true);
            pending[path] = message;
            return;
        }
        message.setDelayedReply(true);
        QTimer::singleShot(device.pairDelay, this, [this, message, path] {
            if (!devices.contains(path)) {
                return;
            }
            const QString error = devices[path].pairError;
            if (!error.isEmpty()) {
                send(message.createErrorReply(error, QStringLiteral("no")));
                return;
            }
            setDevice(path, QStringLiteral("Paired"), true);
            send(message.createReply());
        });
    }

    void connectDevice(const QDBusMessage& message)
    {
        const QString path = message.path();
        message.setDelayedReply(true);
        QTimer::singleShot(devices[path].connectDelay, this, [this, message, path] {
            if (!devices.contains(path)) {
                return;
            }
            const Device& device = devices[path];
            if (!device.connectError.isEmpty()) {
                send(message.createErrorReply(device.connectError, device.connectMessage));
                return;
            }
            if (device.connected) {
                send(message.createErrorReply(QStringLiteral("org.bluez.Error.AlreadyConnected"), QStringLiteral("Already Connected")));
                return;
            }
            setDevice(path, QStringLiteral("Connected"), true);
            send(message.createReply());
        });
    }
};
