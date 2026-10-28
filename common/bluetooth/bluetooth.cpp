// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bluetooth/bluetooth.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDateTime>

#include <algorithm>
#include <ctime>

namespace vela::bluetooth {

namespace {

const QString service = QStringLiteral("org.vela.Bluetooth");
const QString objectPath = QStringLiteral("/org/vela/Bluetooth");
const QString errorPrefix = QStringLiteral("org.vela.Bluetooth.Error.");

// Steps take up to 90 s in the daemon (§6.3); an Add has three of them.
constexpr int callTimeout = 40000;
constexpr int addTimeout = 200000;

QVariantMap mapOf(const QVariant& value)
{
    return value.canConvert<QDBusArgument>() ? qdbus_cast<QVariantMap>(value) : value.toMap();
}

// Object paths inside a dictionary become strings, for QML.
QVariantMap plain(QVariantMap map)
{
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (it->canConvert<QDBusObjectPath>()) {
            *it = it->value<QDBusObjectPath>().path();
        }
    }
    return map;
}

QVariantList mapsOf(const QVariant& value)
{
    QVariantList list;
    const QDBusArgument argument = value.value<QDBusArgument>();
    argument.beginArray();
    while (!argument.atEnd()) {
        QVariantMap map;
        argument >> map;
        list << plain(map);
    }
    argument.endArray();
    return list;
}

QMap<QString, QVariantMap> devicesOf(const QVariant& value)
{
    QMap<QString, QVariantMap> devices;
    const QDBusArgument argument = value.value<QDBusArgument>();
    argument.beginMap();
    while (!argument.atEnd()) {
        QDBusObjectPath path;
        QVariantMap details;
        argument.beginMapEntry();
        argument >> path >> details;
        argument.endMapEntry();
        devices.insert(path.path(), details);
    }
    argument.endMap();
    return devices;
}

qint64 monotonicMs()
{
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

} // namespace

Bluetooth::Bluetooth(bool ui, QObject* parent)
    : Bluetooth(QDBusConnection::sessionBus(), ui, parent)
{
}

Bluetooth::Bluetooth(const QDBusConnection& bus, bool ui, QObject* parent)
    : QObject(parent)
    , m_bus(bus)
    , m_ui(ui)
    , m_power([this] { emit changed(); }, 12000) // an unblocked platform switch's adapter takes up to 10 s
{
    auto* watcher = new QDBusServiceWatcher(service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &Bluetooth::onOwnerChanged);
    m_bus.connect(service, objectPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
        this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    m_bus.connect(service, objectPath, service, QStringLiteral("DevicesChanged"), this, SLOT(onDevicesChanged(QDBusMessage)));
    m_bus.connect(service, objectPath, service, QStringLiteral("SearchEnded"), this, SLOT(onSearchEnded(qulonglong, QString)));
    if (m_ui) {
        // Sent only to the clients that asked for them (§7.3).
        m_bus.connect(service, objectPath, service, QStringLiteral("Question"), this, SLOT(onQuestion(QDBusMessage)));
        m_bus.connect(service, objectPath, service, QStringLiteral("QuestionChanged"), this,
            SLOT(onQuestionChanged(QDBusMessage)));
        m_bus.connect(service, objectPath, service, QStringLiteral("QuestionClosed"), this,
            SLOT(onQuestionClosed(qulonglong, QString)));
    }
    if (m_bus.interface() && m_bus.interface()->isServiceRegistered(service)) {
        appeared();
    }
}

bool Bluetooth::powered() const
{
    return m_power.value(available() && m_adapter.value(QStringLiteral("powered")).toBool());
}

QVariantList Bluetooth::devices() const
{
    const QString waiting = m_question.value(QStringLiteral("device")).toString();
    QVariantList list;
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it) {
        const QVariantMap& d = it.value();
        list << QVariantMap {
            { QStringLiteral("path"), it.key() },
            { QStringLiteral("address"), d.value(QStringLiteral("address")).toString() },
            { QStringLiteral("name"), d.value(QStringLiteral("name")).toString() },
            { QStringLiteral("icon"), d.value(QStringLiteral("icon")).toString() },
            { QStringLiteral("type"), d.value(QStringLiteral("type")).toString() },
            { QStringLiteral("paired"), d.value(QStringLiteral("paired")).toBool() },
            { QStringLiteral("trusted"), d.value(QStringLiteral("trusted")).toBool() },
            { QStringLiteral("connected"), d.value(QStringLiteral("connected")).toBool() },
            { QStringLiteral("blocked"), d.value(QStringLiteral("blocked")).toBool() },
            { QStringLiteral("battery"), d.contains(QStringLiteral("battery")) ? d.value(QStringLiteral("battery")).toInt() : -1 },
            { QStringLiteral("rssi"), d.value(QStringLiteral("rssi")).toInt() },
            { QStringLiteral("operation"), d.value(QStringLiteral("operation")).toString() },
            { QStringLiteral("error"), d.value(QStringLiteral("error")).toString() },
            { QStringLiteral("message"), d.value(QStringLiteral("message")).toString() },
            { QStringLiteral("waiting"), it.key() == waiting },
        };
    }
    return list;
}

QVariantList Bluetooth::paired() const
{
    QVariantList list;
    for (const QVariant& device : devices()) {
        if (device.toMap().value(QStringLiteral("paired")).toBool()) {
            list << device;
        }
    }
    std::stable_sort(list.begin(), list.end(), [](const QVariant& a, const QVariant& b) {
        const QVariantMap x = a.toMap();
        const QVariantMap y = b.toMap();
        const bool xConnected = x.value(QStringLiteral("connected")).toBool();
        const bool yConnected = y.value(QStringLiteral("connected")).toBool();
        if (xConnected != yConnected) {
            return xConnected;
        }
        return QString::localeAwareCompare(x.value(QStringLiteral("name")).toString(), y.value(QStringLiteral("name")).toString()) < 0;
    });
    return list;
}

QVariantList Bluetooth::found() const
{
    QVariantList list;
    for (const QVariant& device : devices()) {
        if (!device.toMap().value(QStringLiteral("paired")).toBool()) {
            list << device;
        }
    }
    // Nearest first; those without a signal strength last.
    std::stable_sort(list.begin(), list.end(), [](const QVariant& a, const QVariant& b) {
        const QVariantMap x = a.toMap();
        const QVariantMap y = b.toMap();
        const int xRssi = x.value(QStringLiteral("rssi")).toInt();
        const int yRssi = y.value(QStringLiteral("rssi")).toInt();
        if ((xRssi == 0) != (yRssi == 0)) {
            return yRssi == 0;
        }
        if (xRssi != yRssi) {
            return xRssi > yRssi;
        }
        return QString::localeAwareCompare(x.value(QStringLiteral("name")).toString(), y.value(QStringLiteral("name")).toString()) < 0;
    });
    return list;
}

QString Bluetooth::pairing() const
{
    if (!m_adding.isEmpty()) {
        return m_adding;
    }
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it) {
        const QString operation = it->value(QStringLiteral("operation")).toString();
        if (operation == QLatin1String("pairing") || operation == QLatin1String("trusting")) {
            return it.key();
        }
    }
    if (m_question.value(QStringLiteral("unsolicited")).toBool()) {
        return m_question.value(QStringLiteral("device")).toString();
    }
    return {};
}

// ------------------------------------------------------------ actions --

void Bluetooth::setPowered(bool on)
{
    if (!available() && !platformBlocked()) {
        return;
    }
    clearError();
    const Requested::Id id = m_power.ask(on);
    call(QStringLiteral("SetPowered"), { on }, callTimeout, [this, id](const QDBusMessage& reply) {
        if (reply.type() == QDBusMessage::ErrorMessage) {
            m_power.refused(id);
        }
    });
}

void Bluetooth::useAdapter(const QString& address)
{
    clearError();
    call(QStringLiteral("UseAdapter"), { address }, callTimeout);
}

void Bluetooth::add(const QString& path)
{
    clearError();
    m_adding = path;
    emit changed();
    call(QStringLiteral("Add"), { QVariant::fromValue(QDBusObjectPath(path)), m_ui }, addTimeout,
        [this, path](const QDBusMessage& reply) {
            if (m_adding == path) {
                m_adding.clear();
                emit changed();
            }
            if (reply.type() != QDBusMessage::ErrorMessage) {
                emit added(path);
            }
        },
        Refusal::OnDevice);
}

void Bluetooth::connectDevice(const QString& path)
{
    deviceCall(QStringLiteral("Connect"), path);
}

void Bluetooth::disconnectDevice(const QString& path)
{
    deviceCall(QStringLiteral("Disconnect"), path);
}

void Bluetooth::removeDevice(const QString& path)
{
    deviceCall(QStringLiteral("Remove"), path);
}

void Bluetooth::rename(const QString& path, const QString& name)
{
    deviceCall(QStringLiteral("Rename"), path, { name });
}

void Bluetooth::cancel(const QString& path)
{
    deviceCall(QStringLiteral("Cancel"), path);
}

void Bluetooth::answer(const QString& id, const QString& response, const QString& value)
{
    clearError();
    call(QStringLiteral("Answer"), { id.toULongLong(), response, value }, callTimeout);
}

void Bluetooth::startSearch()
{
    m_searchWanted = true;
    if (m_lease != 0 || m_searchStarting) {
        return;
    }
    m_searchStarting = true;
    m_searchEnded = false;
    emit searchChanged();
    call(QStringLiteral("StartSearch"), { true }, callTimeout, [this](const QDBusMessage& reply) {
        m_searchStarting = false;
        if (reply.type() == QDBusMessage::ErrorMessage) {
            // A daemon without searches yet shows the devices BlueZ knows.
            endSearch(reply.errorName() != QLatin1String("org.freedesktop.DBus.Error.UnknownMethod"));
            return;
        }
        const quint64 lease = reply.arguments().value(0).toULongLong();
        if (!m_searchWanted) {
            call(QStringLiteral("StopSearch"), { lease }, callTimeout);
            return;
        }
        m_lease = lease;
        emit searchChanged();
    });
}

void Bluetooth::stopSearch()
{
    m_searchWanted = false;
    if (m_lease != 0) {
        call(QStringLiteral("StopSearch"), { qulonglong(m_lease) }, callTimeout);
        endSearch(false);
    }
}

void Bluetooth::stopBeingVisible()
{
    clearError();
    call(QStringLiteral("StopBeingVisible"), {}, callTimeout);
}

void Bluetooth::clearError()
{
    setError({});
}

// --------------------------------------------------------- the daemon --

QDBusMessage Bluetooth::method(const QString& name, const QVariantList& arguments) const
{
    QDBusMessage message = QDBusMessage::createMethodCall(service, objectPath, service, name);
    message.setArguments(arguments);
    return message;
}

void Bluetooth::call(const QString& name, const QVariantList& arguments, int timeout,
    const std::function<void(const QDBusMessage&)>& done, Refusal refusal)
{
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(method(name, arguments), timeout), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, done, name, refusal] {
        watcher->deleteLater();
        const QDBusMessage reply = watcher->reply();
        if (reply.type() == QDBusMessage::ErrorMessage && refusal == Refusal::Logged) {
            qWarning("vela-bluetooth: %s: %s: %s", qPrintable(name), qPrintable(reply.errorName()), qPrintable(reply.errorMessage()));
        } else if (reply.type() == QDBusMessage::ErrorMessage) {
            refused(reply, refusal == Refusal::OnDevice);
        }
        if (done) {
            done(reply);
        }
    });
}

void Bluetooth::deviceCall(const QString& name, const QString& path, const QVariantList& extra)
{
    clearError();
    call(name, QVariantList { QVariant::fromValue(QDBusObjectPath(path)) } + extra, callTimeout, {}, Refusal::OnDevice);
}

void Bluetooth::refused(const QDBusMessage& reply, bool onDevice)
{
    const QString name = reply.errorName();
    if (name == errorPrefix + QLatin1String("Cancelled") || name == errorPrefix + QLatin1String("UnknownQuestion")
        || name == errorPrefix + QLatin1String("QuestionClosed") ) {
        return;
    }
    if (name == errorPrefix + QLatin1String("Busy")) {
        const QString busy = m_devices.value(reply.errorMessage()).value(QStringLiteral("name")).toString();
        setError(busy.isEmpty() ? tr("Bluetooth is busy. Try again in a moment.") : tr("Wait for %1 to finish pairing.").arg(busy));
    } else if (name == errorPrefix + QLatin1String("Failed")) {
        // A device's failure is shown with the device (its error and
        // message).
        if (!onDevice) {
            setError(reply.errorMessage());
        }
    } else if (name == errorPrefix + QLatin1String("Inactive")) {
        setError(tr("Bluetooth can't be changed while the screen is locked or another session is in use."));
    } else if (name == errorPrefix + QLatin1String("NotAllowed")) {
        setError(tr("This app isn't allowed to change Bluetooth."));
    } else if (name == errorPrefix + QLatin1String("UnknownDevice")) {
        setError(tr("The device isn't there anymore."));
    } else if (name == errorPrefix + QLatin1String("InvalidValue")) {
        setError(tr("That code isn't valid."));
    } else if (name == QLatin1String("org.freedesktop.DBus.Error.InvalidArgs")) {
        setError(tr("That name can't be used. Use up to 248 characters, without control characters."));
    } else {
        setError(tr("Bluetooth isn't responding."));
    }
}

void Bluetooth::setError(const QString& error)
{
    if (m_error != error) {
        m_error = error;
        emit errorChanged();
    }
}

void Bluetooth::onOwnerChanged(const QString&, const QString& oldOwner, const QString& newOwner)
{
    if (!oldOwner.isEmpty()) {
        vanished();
    }
    if (!newOwner.isEmpty()) {
        appeared();
    }
}

void Bluetooth::appeared()
{
    m_running = true;
    QDBusMessage getAll = QDBusMessage::createMethodCall(service, objectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("GetAll"));
    getAll << service;
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(getAll, callTimeout), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        watcher->deleteLater();
        const QDBusMessage reply = watcher->reply();
        if (reply.type() == QDBusMessage::ReplyMessage) {
            setProperties(mapOf(reply.arguments().value(0)));
        }
    });
    // Reading isn't the user's doing: a refusal goes to the log.
    call(
        QStringLiteral("Devices"), {}, callTimeout,
        [this](const QDBusMessage& reply) {
            if (reply.type() == QDBusMessage::ReplyMessage) {
                m_devices = devicesOf(reply.arguments().value(0));
                emit changed();
            }
        },
        Refusal::Logged);
    if (m_ui) {
        call(
            QStringLiteral("Questions"), {}, callTimeout,
            [this](const QDBusMessage& reply) {
                if (reply.type() != QDBusMessage::ReplyMessage) {
                    return;
                }
                const QDBusArgument argument = reply.arguments().value(0).value<QDBusArgument>();
                argument.beginArray();
                while (!argument.atEnd()) {
                    qulonglong id = 0;
                    QDBusObjectPath device;
                    QVariantMap details;
                    argument.beginStructure();
                    argument >> id >> device >> details;
                    argument.endStructure();
                    setQuestion(id, device.path(), details);
                }
                argument.endArray();
            },
            Refusal::Logged);
    }
}

// The daemon left the bus: nothing it knew holds anymore, and its
// questions and lease went with it (§7.2).
void Bluetooth::vanished()
{
    m_running = false;
    m_available = false;
    m_platformBlocked = false;
    m_active = true;
    m_adapter.clear();
    m_adapters.clear();
    m_devices.clear();
    m_adding.clear();
    dropQuestion();
    if (m_lease != 0) {
        endSearch(true);
    }
    m_power.reported(false);
    clearError();
    emit changed();
}

void Bluetooth::onPropertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList&)
{
    if (interface == service) {
        setProperties(changed);
    }
}

void Bluetooth::setProperties(const QVariantMap& values)
{
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key() == QLatin1String("Available")) {
            m_available = it->toBool();
        } else if (it.key() == QLatin1String("PlatformBlocked")) {
            m_platformBlocked = it->toBool();
        } else if (it.key() == QLatin1String("Active")) {
            m_active = it->toBool();
        } else if (it.key() == QLatin1String("Adapters")) {
            m_adapters = mapsOf(*it);
        } else if (it.key() == QLatin1String("Adapter")) {
            m_adapter = plain(mapOf(*it));
        }
    }
    m_power.reported(available() && m_adapter.value(QStringLiteral("powered")).toBool());
    emit changed();
}

void Bluetooth::onDevicesChanged(const QDBusMessage& message)
{
    const QMap<QString, QVariantMap> changedDevices = devicesOf(message.arguments().value(0));
    for (auto it = changedDevices.cbegin(); it != changedDevices.cend(); ++it) {
        m_devices.insert(it.key(), it.value());
    }
    const QDBusArgument removed = message.arguments().value(1).value<QDBusArgument>();
    removed.beginArray();
    while (!removed.atEnd()) {
        QDBusObjectPath path;
        removed >> path;
        m_devices.remove(path.path());
    }
    removed.endArray();
    emit changed();
}

// ---------------------------------------------------------- questions --

void Bluetooth::setQuestion(qulonglong id, const QString& device, const QVariantMap& details)
{
    QVariantMap question;
    for (auto it = details.cbegin(); it != details.cend(); ++it) {
        if (it.key() == QLatin1String("passkey")) {
            question.insert(it.key(), QStringLiteral("%1").arg(it->toUInt(), 6, 10, QLatin1Char('0')));
        } else if (it.key() == QLatin1String("expires")) {
            const qint64 left = qint64(it->toULongLong() / 1000) - monotonicMs();
            question.insert(QStringLiteral("deadline"), double(QDateTime::currentMSecsSinceEpoch() + left));
        } else if (it.key() == QLatin1String("entered")) {
            question.insert(it.key(), it->toInt());
        } else {
            question.insert(it.key(), *it);
        }
    }
    question.insert(QStringLiteral("id"), QString::number(id));
    question.insert(QStringLiteral("device"), device);
    m_question = question;
    emit questionChanged();
    emit changed();
}

void Bluetooth::dropQuestion()
{
    if (m_question.isEmpty()) {
        return;
    }
    const QString id = m_question.value(QStringLiteral("id")).toString();
    m_question.clear();
    emit questionChanged();
    emit changed();
    emit questionClosed(id, QStringLiteral("bluetooth-gone"));
}

void Bluetooth::onQuestion(const QDBusMessage& message)
{
    const QVariantList arguments = message.arguments();
    setQuestion(arguments.value(0).toULongLong(), arguments.value(1).value<QDBusObjectPath>().path(), mapOf(arguments.value(2)));
}

void Bluetooth::onQuestionChanged(const QDBusMessage& message)
{
    const QVariantList arguments = message.arguments();
    if (QString::number(arguments.value(0).toULongLong()) != m_question.value(QStringLiteral("id")).toString()) {
        return;
    }
    const QVariantMap details = mapOf(arguments.value(1));
    if (details.contains(QStringLiteral("entered"))) {
        m_question.insert(QStringLiteral("entered"), details.value(QStringLiteral("entered")).toInt());
        emit questionChanged();
    }
}

void Bluetooth::onQuestionClosed(qulonglong id, const QString& reason)
{
    const QString closed = QString::number(id);
    if (closed != m_question.value(QStringLiteral("id")).toString()) {
        return;
    }
    m_question.clear();
    emit questionChanged();
    emit changed();
    emit questionClosed(closed, reason);
}

// ----------------------------------------------------------- searches --

void Bluetooth::onSearchEnded(qulonglong lease, const QString&)
{
    if (lease == m_lease) {
        m_searchWanted = false;
        endSearch(true);
    }
}

void Bluetooth::endSearch(bool ended)
{
    m_lease = 0;
    m_searchEnded = ended;
    emit searchChanged();
}

} // namespace vela::bluetooth
