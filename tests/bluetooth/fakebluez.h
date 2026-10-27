// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A fake BlueZ for the Bluetooth tests (docs/bluetooth.md §11), on a private
// system bus. It serves the ObjectManager, Adapter1, Device1, Battery1 and
// AgentManager1, and each device follows a script: how it pairs (which
// agent request it makes), how pairing ends and after how long, whether
// Connect and Set Trusted work. Agents are kept as BlueZ keeps them: a
// default-agent stack, and one open request per agent, a second one
// refused as busy. Devices can also ask by themselves, through the default
// agent. It records the calls it gets, every property written, and what
// each agent answered, so the tests can check what vela-bluetooth did.
//
// FakeLogind is the logind session the daemon follows (§6.7), with Active
// and LockedHint.
//
// Both live in a thread of their own (the daemon under test calls them
// while the test waits); the test reaches them through run().

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
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
        // How it pairs: Just Works (no request), a passkey to confirm, a
        // passkey typed on this PC, a passkey shown here and typed on the
        // device, a legacy PIN typed here (it accepts `pin`, any if empty),
        // or a PIN BlueZ makes and the agent shows (DisplayPinCode).
        enum { JustWorks, Confirm, EnterPasskey, ShowPasskey, LegacyPin, ShowPin } method = JustWorks;
        quint32 passkey = 123456;
        QString pin;
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
    QStringList calls; // "Pair /org/bluez/hci0/dev_...", "RegisterAgent KeyboardDisplay"
    QStringList writes; // "/org/bluez/hci0 Powered=true"
    // What the agents were asked and answered:
    // "RequestConfirmation /org/bluez/hci0/dev_... -> ok", "... -> busy".
    QStringList agentLog;
    // The default-agent stack, as "owner path": the last is the default.
    QStringList defaults;
    QString bus;

    QString defaultAgent() const { return defaults.isEmpty() ? QString() : defaults.last(); }

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
        if (interface == QLatin1String("org.bluez.AgentManager1") && path == QLatin1String("/org/bluez")) {
            return agentManager(message);
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
                cancelRequestOn(path);
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
        // Its request goes after it.
        cancelRequestOn(path);
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

    // bluetoothd stops (`leave`), releasing its agents, and starts again
    // (`come back`) without any.
    bool leave()
    {
        for (const QString& agent : agents.keys()) {
            QDBusMessage release = QDBusMessage::createMethodCall(
                agent.section(u' ', 0, 0), agent.section(u' ', 1), QStringLiteral("org.bluez.Agent1"), QStringLiteral("Release"));
            send(release);
        }
        agents.clear();
        defaults.clear();
        requests.clear();
        return connection().unregisterService(QStringLiteral("org.bluez"));
    }
    bool comeBack() { return connection().registerService(QStringLiteral("org.bluez")); }

    // Forgets every agent, as when each test starts.
    void forgetAgents()
    {
        agents.clear();
        defaults.clear();
        requests.clear();
        agentLog.clear();
    }

    // The device asks by itself, through the default agent: `method`
    // ("RequestConfirmation", "RequestAuthorization", "AuthorizeService",
    // "RequestPinCode", "RequestPasskey", "DisplayPasskey") with `extra`
    // after the device. Accepting a pairing request pairs the device.
    void ask(const QString& path, const QString& method, const QVariantList& extra = {})
    {
        const QString agent = defaultAgent();
        if (agent.isEmpty()) {
            agentLog << method + u' ' + path + QStringLiteral(" -> no agent");
            return;
        }
        QVariantList arguments { QVariant::fromValue(QDBusObjectPath(path)) };
        arguments += extra;
        if (method == QLatin1String("DisplayPasskey")) {
            display(agent, path, arguments);
            return;
        }
        askAgent(agent, path, method, arguments, [this, path, method](const QDBusMessage& reply) {
            if (reply.type() != QDBusMessage::ErrorMessage && method != QLatin1String("AuthorizeService") && devices.contains(path)) {
                setDevice(path, QStringLiteral("Paired"), true);
            }
        });
    }

    // BlueZ gives up on the agent's open request about `path` (its
    // timeout, the device going): Cancel goes to the agent.
    void cancelRequestOn(const QString& path)
    {
        for (auto it = requests.begin(); it != requests.end(); ++it) {
            if (it->device == path) {
                const QString agent = it.key();
                agentLog << it->method + u' ' + path + QStringLiteral(" -> cancelled");
                requests.erase(it);
                send(QDBusMessage::createMethodCall(
                    agent.section(u' ', 0, 0), agent.section(u' ', 1), QStringLiteral("org.bluez.Agent1"), QStringLiteral("Cancel")));
                return;
            }
        }
    }

    // Calls the agent at `owner path` as BlueZ would, for a test that
    // pretends to be BlueZ's impostor. The reply, or the error name.
    QString callAgent(const QString& agent, const QString& method, const QVariantList& arguments)
    {
        QDBusMessage call = QDBusMessage::createMethodCall(
            agent.section(u' ', 0, 0), agent.section(u' ', 1), QStringLiteral("org.bluez.Agent1"), method);
        call.setArguments(arguments);
        const QDBusMessage reply = connection().call(call, QDBus::Block, 5000);
        return reply.type() == QDBusMessage::ErrorMessage ? reply.errorName() : QStringLiteral("ok");
    }

private:
    QMap<QString, QDBusMessage> pending; // Pair waiting for its reply, by device
    QMap<QString, QString> agents; // "owner path" -> capability

    // The one open request of an agent, by "owner path" (§6.5).
    struct Request {
        QString device;
        QString method;
        quint64 serial = 0;
    };
    QMap<QString, Request> requests;
    quint64 serial = 0;

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

    bool agentManager(const QDBusMessage& message)
    {
        const QString member = message.member();
        const QString agent = message.service() + u' ' + message.arguments().value(0).value<QDBusObjectPath>().path();
        if (member == QLatin1String("RegisterAgent")) {
            const QString capability = message.arguments().value(1).toString();
            calls << QStringLiteral("RegisterAgent ") + capability;
            if (agents.contains(agent)) {
                send(message.createErrorReply(QStringLiteral("org.bluez.Error.AlreadyExists"), QStringLiteral("Already Exists")));
                return true;
            }
            agents[agent] = capability;
            // BlueZ forgets the agents of a client that leaves the bus.
            auto* watcher = new QDBusServiceWatcher(message.service(), connection(), QDBusServiceWatcher::WatchForUnregistration, this);
            connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this, watcher, owner = message.service()] {
                watcher->deleteLater();
                for (const QString& key : agents.keys()) {
                    if (key.section(u' ', 0, 0) == owner) {
                        forget(key);
                    }
                }
            });
        } else if (member == QLatin1String("UnregisterAgent")) {
            calls << QStringLiteral("UnregisterAgent");
            if (!agents.contains(agent)) {
                send(message.createErrorReply(QStringLiteral("org.bluez.Error.DoesNotExist"), QStringLiteral("Does Not Exist")));
                return true;
            }
            forget(agent);
            send(QDBusMessage::createMethodCall(
                message.service(), agent.section(u' ', 1), QStringLiteral("org.bluez.Agent1"), QStringLiteral("Release")));
        } else if (member == QLatin1String("RequestDefaultAgent")) {
            calls << QStringLiteral("RequestDefaultAgent");
            if (!agents.contains(agent)) {
                send(message.createErrorReply(QStringLiteral("org.bluez.Error.DoesNotExist"), QStringLiteral("Does Not Exist")));
                return true;
            }
            defaults.removeAll(agent);
            defaults << agent;
        } else {
            return false;
        }
        send(message.createReply());
        return true;
    }

    // The previous default agent is default again (remove_default_agent).
    void forget(const QString& agent)
    {
        agents.remove(agent);
        defaults.removeAll(agent);
        requests.remove(agent);
    }

    // The agent of Pair's caller, else the default one (agent_get).
    QString agentFor(const QString& caller) const
    {
        for (const QString& key : agents.keys()) {
            if (key.section(u' ', 0, 0) == caller) {
                return key;
            }
        }
        return defaultAgent();
    }

    // One request to `agent`; `done` gets its reply. Refused at once if the
    // agent has one open already (-EBUSY in src/agent.c).
    bool askAgent(const QString& agent, const QString& path, const QString& method, const QVariantList& arguments,
        const std::function<void(const QDBusMessage&)>& done)
    {
        if (agent.isEmpty()) {
            agentLog << method + u' ' + path + QStringLiteral(" -> no agent");
            return false;
        }
        if (requests.contains(agent)) {
            agentLog << method + u' ' + path + QStringLiteral(" -> busy");
            return false;
        }
        QDBusMessage call = QDBusMessage::createMethodCall(
            agent.section(u' ', 0, 0), agent.section(u' ', 1), QStringLiteral("org.bluez.Agent1"), method);
        call.setArguments(arguments);
        const quint64 id = ++serial;
        requests[agent] = Request { path, method, id };
        auto* watcher = new QDBusPendingCallWatcher(connection().asyncCall(call, 120000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, agent, path, method, id, done] {
            watcher->deleteLater();
            // Cancelled meanwhile: BlueZ ignores a late reply.
            if (!requests.contains(agent) || requests[agent].serial != id) {
                return;
            }
            requests.remove(agent);
            const QDBusMessage reply = watcher->reply();
            QString answer = QStringLiteral("ok");
            if (reply.type() == QDBusMessage::ErrorMessage) {
                answer = reply.errorName();
            } else if (!reply.arguments().isEmpty()) {
                answer = reply.arguments().value(0).toString();
            }
            agentLog << method + u' ' + path + QStringLiteral(" -> ") + answer;
            done(reply);
        });
        return true;
    }

    // DisplayPasskey expects no reply.
    void display(const QString& agent, const QString& path, const QVariantList& arguments)
    {
        if (agent.isEmpty()) {
            return;
        }
        QDBusMessage call = QDBusMessage::createMethodCall(
            agent.section(u' ', 0, 0), agent.section(u' ', 1), QStringLiteral("org.bluez.Agent1"), QStringLiteral("DisplayPasskey"));
        call.setArguments(arguments);
        send(call);
        agentLog << QStringLiteral("DisplayPasskey ") + path + u' ' + arguments.value(2).toString();
    }

    // Pair's reply: `error` empty, paired.
    void finishPair(const QString& path, const QString& error)
    {
        if (!pending.contains(path)) {
            return;
        }
        const QDBusMessage message = pending.take(path);
        if (!error.isEmpty()) {
            send(message.createErrorReply(error, QStringLiteral("no")));
            return;
        }
        setDevice(path, QStringLiteral("Paired"), true);
        send(message.createReply());
    }

    // Pair after the agent's reply: a refusal of the agent's is the
    // pairing's (AuthenticationRejected, AuthenticationCanceled).
    void pairAfter(const QString& path, const QDBusMessage& reply, bool accepted)
    {
        if (reply.type() == QDBusMessage::ErrorMessage) {
            finishPair(path,
                reply.errorName() == QLatin1String("org.bluez.Error.Rejected")       ? QStringLiteral("org.bluez.Error.AuthenticationRejected")
                    : reply.errorName() == QLatin1String("org.bluez.Error.Canceled") ? QStringLiteral("org.bluez.Error.AuthenticationCanceled")
                                                                                       : QStringLiteral("org.bluez.Error.AuthenticationFailed"));
        } else {
            finishPair(path, accepted ? QString() : QStringLiteral("org.bluez.Error.AuthenticationFailed"));
        }
    }

    void pair(const QDBusMessage& message)
    {
        const QString path = message.path();
        const Device& device = devices[path];
        if (device.paired) {
            send(message.createErrorReply(QStringLiteral("org.bluez.Error.AlreadyExists"), QStringLiteral("Already Exists")));
            return;
        }
        message.setDelayedReply(true);
        pending[path] = message;
        if (device.pairHangs) {
            return;
        }
        const QString agent = agentFor(message.service());
        const QVariant object = QVariant::fromValue(QDBusObjectPath(path));
        const quint32 passkey = device.passkey;
        const QString pin = device.pin;
        bool asked = true;
        switch (device.method) {
        case Device::JustWorks:
            QTimer::singleShot(device.pairDelay, this, [this, path] {
                if (devices.contains(path)) {
                    finishPair(path, devices[path].pairError);
                }
            });
            return;
        case Device::Confirm:
            asked = askAgent(agent, path, QStringLiteral("RequestConfirmation"), { object, passkey },
                [this, path](const QDBusMessage& reply) { pairAfter(path, reply, true); });
            break;
        case Device::EnterPasskey:
            asked = askAgent(agent, path, QStringLiteral("RequestPasskey"), { object },
                [this, path, passkey](const QDBusMessage& reply) { pairAfter(path, reply, reply.arguments().value(0).toUInt() == passkey); });
            break;
        case Device::LegacyPin:
            asked = askAgent(agent, path, QStringLiteral("RequestPinCode"), { object }, [this, path, pin](const QDBusMessage& reply) {
                pairAfter(path, reply, pin.isEmpty() || reply.arguments().value(0).toString() == pin);
            });
            break;
        case Device::ShowPin:
            // BlueZ gives the PIN to the kernel once the agent replied; the
            // user then types it on the device.
            asked = askAgent(agent, path, QStringLiteral("DisplayPinCode"), { object, pin }, [this, path](const QDBusMessage& reply) {
                if (reply.type() == QDBusMessage::ErrorMessage) {
                    pairAfter(path, reply, false);
                } else if (devices.contains(path)) {
                    QTimer::singleShot(devices[path].pairDelay, this, [this, path] { finishPair(path, QString()); });
                }
            });
            break;
        case Device::ShowPasskey:
            // The user types it on the device: the digits arrive, then the
            // pairing ends.
            display(agent, path, { object, passkey, QVariant::fromValue(quint16(0)) });
            QTimer::singleShot(device.pairDelay / 2, this, [this, agent, path, object, passkey] {
                if (pending.contains(path)) {
                    display(agent, path, { object, passkey, QVariant::fromValue(quint16(3)) });
                }
            });
            QTimer::singleShot(device.pairDelay, this, [this, path] { finishPair(path, QString()); });
            return;
        }
        if (!asked) {
            finishPair(path, QStringLiteral("org.bluez.Error.AuthenticationFailed"));
        }
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

// logind's Manager.GetSession and one session, with Active and LockedHint
// (docs/bluetooth.md §6.7). Every session id names it.
class FakeLogind : public QDBusVirtualObject {
public:
    static constexpr const char* sessionPath = "/org/freedesktop/login1/session/fake";
    bool active = true;
    bool locked = false;
    QString bus;

    QDBusConnection connection() const { return QDBusConnection(bus); }

    // The user switches to another console or user (`active` false), or
    // the lock screen comes up (`locked`).
    void set(bool nowActive, bool nowLocked)
    {
        QVariantMap changed;
        if (nowActive != active) {
            changed[QStringLiteral("Active")] = nowActive;
        }
        if (nowLocked != locked) {
            changed[QStringLiteral("LockedHint")] = nowLocked;
        }
        active = nowActive;
        locked = nowLocked;
        if (changed.isEmpty()) {
            return;
        }
        QDBusMessage signal = QDBusMessage::createSignal(QLatin1String(sessionPath), QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        signal << QStringLiteral("org.freedesktop.login1.Session") << changed << QStringList();
        connection().send(signal);
    }

    QString introspect(const QString&) const override { return {}; }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection&) override
    {
        if (message.member() == QLatin1String("GetSession") && message.path() == QLatin1String("/org/freedesktop/login1")) {
            connection().send(message.createReply(QVariant::fromValue(QDBusObjectPath(QLatin1String(sessionPath)))));
            return true;
        }
        if (message.path() != QLatin1String(sessionPath) || message.interface() != QLatin1String("org.freedesktop.DBus.Properties")) {
            return false;
        }
        const QVariantMap properties { { QStringLiteral("Id"), QStringLiteral("fake") }, { QStringLiteral("Active"), active },
            { QStringLiteral("LockedHint"), locked } };
        if (message.member() == QLatin1String("GetAll")) {
            connection().send(message.createReply(QVariant::fromValue(properties)));
            return true;
        }
        if (message.member() == QLatin1String("Get")) {
            connection().send(message.createReply(QVariant::fromValue(QDBusVariant(properties.value(message.arguments().value(1).toString())))));
            return true;
        }
        return false;
    }
};
