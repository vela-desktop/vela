// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "updates/updates.h"

#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QHash>

#include <tuple>
#include <utility>

namespace vela::updates {

namespace {

const QString service = QStringLiteral("org.vela.Updates");
const QString objectPath = QStringLiteral("/org/vela/Updates");
const QString errorPrefix = QStringLiteral("org.vela.Updates.Error.");

// The daemon answers at once: checks and installs go on after the reply.
constexpr int callTimeout = 10000;

QVariantMap mapOf(const QVariant& value)
{
    return value.canConvert<QDBusArgument>() ? qdbus_cast<QVariantMap>(value) : value.toMap();
}

// (ss)
std::pair<QString, QString> pairOf(const QVariant& value)
{
    std::pair<QString, QString> pair;
    const QDBusArgument argument = value.value<QDBusArgument>();
    argument.beginStructure();
    argument >> pair.first >> pair.second;
    argument.endStructure();
    return pair;
}

// a(ss), as [{first: ..., second: ...}].
QVariantList pairsOf(const QVariant& value, const QString& first, const QString& second)
{
    QVariantList list;
    const QDBusArgument argument = value.value<QDBusArgument>();
    argument.beginArray();
    while (!argument.atEnd()) {
        QString a, b;
        argument.beginStructure();
        argument >> a >> b;
        argument.endStructure();
        list << QVariantMap { { first, a }, { second, b } };
    }
    argument.endArray();
    return list;
}

} // namespace

Updates::Updates(QObject* parent)
    : Updates(QDBusConnection::sessionBus(), parent)
{
}

Updates::Updates(const QDBusConnection& bus, QObject* parent)
    : QObject(parent)
    , m_bus(bus)
    , m_autoCheckAsked([this] { emit changed(); })
    , m_checkIntervalAsked([this] { emit changed(); })
{
    forget();
    auto* watcher = new QDBusServiceWatcher(service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &Updates::onOwnerChanged);
    m_bus.connect(service, objectPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
        this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    if (m_bus.interface() && m_bus.interface()->isServiceRegistered(service)) {
        appeared();
    }
}

QString Updates::versionText() const
{
    if (m_channel == QLatin1String("experimental") && !m_commit.isEmpty()) {
        return QStringLiteral("%1 (%2)").arg(m_version, m_commit.left(7));
    }
    return m_version;
}

QDateTime Updates::lastCheck() const
{
    return m_lastCheck > 0 ? QDateTime::fromSecsSinceEpoch(m_lastCheck) : QDateTime();
}

QDateTime Updates::nextCheck() const
{
    return m_nextCheck > 0 ? QDateTime::fromSecsSinceEpoch(m_nextCheck) : QDateTime();
}

QDateTime Updates::installFinished() const
{
    return m_installFinished > 0 ? QDateTime::fromSecsSinceEpoch(m_installFinished) : QDateTime();
}

QString Updates::reason(const QString& code) const
{
    if (code == QLatin1String("busy")) {
        return tr("Another update is already running.");
    }
    if (code == QLatin1String("unsupported")) {
        return tr("Vela can't update itself on this system.");
    }
    if (code == QLatin1String("missing-tool")) {
        return tr("A program needed to update Vela isn't installed.");
    }
    if (code == QLatin1String("network")) {
        return tr("Couldn't connect. Check your internet connection.");
    }
    if (code == QLatin1String("rate-limited")) {
        return tr("GitHub is limiting requests. Try again later.");
    }
    if (code == QLatin1String("not-found")) {
        // Until the first Beta release (§12).
        return m_channel == QLatin1String("beta") ? tr("No Beta release has been published yet.")
                                                   : tr("The update wasn't found.");
    }
    if (code == QLatin1String("downgrade-refused")) {
        return tr("The installed version is newer than the update.");
    }
    if (code == QLatin1String("verification-failed")) {
        return tr("The update couldn't be verified, so it wasn't installed.");
    }
    if (code == QLatin1String("build-failed")) {
        return tr("Building the update failed.");
    }
    if (code == QLatin1String("install-failed")) {
        return tr("The package manager didn't install the update.");
    }
    if (code == QLatin1String("cancelled")) {
        return tr("The update was interrupted.");
    }
    if (code == QLatin1String("terminal")) {
        return tr("The terminal didn't start the update.");
    }
    return tr("Something went wrong.");
}

// ------------------------------------------------------------ actions --

void Updates::check()
{
    if (!available()) {
        return;
    }
    clearError();
    call(QStringLiteral("Check"), {}, true);
}

void Updates::install(const QVariantMap& options)
{
    if (!available()) {
        return;
    }
    clearError();
    // a{sv} of strings; an empty one is no option.
    QVariantMap strings;
    for (auto it = options.cbegin(); it != options.cend(); ++it) {
        const QString value = it->toString();
        if (!value.isEmpty()) {
            strings.insert(it.key(), value);
        }
    }
    call(QStringLiteral("Install"), { strings }, true);
}

void Updates::setAutoCheck(bool on)
{
    if (!available()) {
        return;
    }
    clearError();
    const Requested::Id id = m_autoCheckAsked.ask(on);
    call(QStringLiteral("SetAutoCheck"), { on }, true, [this, id](const QDBusMessage& reply) {
        if (reply.type() == QDBusMessage::ErrorMessage) {
            m_autoCheckAsked.refused(id);
        }
    });
}

void Updates::setCheckInterval(uint hours)
{
    if (!available()) {
        return;
    }
    clearError();
    const Requested::Id id = m_checkIntervalAsked.ask(hours);
    call(QStringLiteral("SetCheckInterval"), { hours }, true, [this, id](const QDBusMessage& reply) {
        if (reply.type() == QDBusMessage::ErrorMessage) {
            m_checkIntervalAsked.refused(id);
        }
    });
}

void Updates::announced(const QString& commit)
{
    if (available() && !commit.isEmpty()) {
        call(QStringLiteral("Announced"), { commit }, false);
    }
}

void Updates::clearError()
{
    setError({});
}

// --------------------------------------------------------- the daemon --

void Updates::call(const QString& name, const QVariantList& arguments, bool shown,
    const std::function<void(const QDBusMessage&)>& done)
{
    QDBusMessage message = QDBusMessage::createMethodCall(service, objectPath, service, name);
    message.setArguments(arguments);
    const quint64 generation = m_ownerGeneration;
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, callTimeout), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, done, name, shown, generation] {
        watcher->deleteLater();
        if (generation != m_ownerGeneration) {
            return;
        }
        const QDBusMessage reply = watcher->reply();
        if (reply.type() == QDBusMessage::ErrorMessage && !shown) {
            qWarning("vela-update-daemon: %s: %s: %s", qPrintable(name), qPrintable(reply.errorName()),
                qPrintable(reply.errorMessage()));
        } else if (reply.type() == QDBusMessage::ErrorMessage) {
            refused(reply);
        }
        if (done) {
            done(reply);
        }
    });
}

void Updates::refused(const QDBusMessage& reply)
{
    const QString name = reply.errorName();
    if (name == errorPrefix + QLatin1String("Busy")) {
        setError(tr("An update or a check is already running."));
    } else if (name == errorPrefix + QLatin1String("InvalidArgs")) {
        // Which option, in vela-update's own words.
        setError(tr("Vela can't update to that: %1").arg(reply.errorMessage()));
    } else if (name == errorPrefix + QLatin1String("NoTerminal")) {
        setError(tr("Couldn't open a terminal to run the update."));
    } else if (name == errorPrefix + QLatin1String("NotAllowed")) {
        setError(tr("This app isn't allowed to update Vela."));
    } else if (name == errorPrefix + QLatin1String("Failed")) {
        setError(tr("Couldn't start vela-update."));
    } else {
        setError(tr("The update service isn't responding."));
    }
}

void Updates::setError(const QString& error)
{
    if (m_error != error) {
        m_error = error;
        emit errorChanged();
    }
}

QVariantMap Updates::failure(const QString& code, const QString& message) const
{
    if (code.isEmpty()) {
        return {};
    }
    return { { QStringLiteral("code"), code }, { QStringLiteral("message"), message },
        { QStringLiteral("reason"), reason(code) } };
}

void Updates::onOwnerChanged(const QString&, const QString& oldOwner, const QString& newOwner)
{
    if (!oldOwner.isEmpty()) {
        vanished();
    }
    if (!newOwner.isEmpty()) {
        appeared();
    }
}

void Updates::appeared()
{
    const quint64 generation = ++m_ownerGeneration;
    m_running = true;
    QDBusMessage getAll = QDBusMessage::createMethodCall(service, objectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("GetAll"));
    getAll << service;
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(getAll, callTimeout), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_ownerGeneration) {
            return;
        }
        const QDBusMessage reply = watcher->reply();
        if (reply.type() == QDBusMessage::ReplyMessage) {
            setProperties(mapOf(reply.arguments().value(0)));
        }
    });
    emit changed();
}

// The daemon left the bus: nothing it knew holds anymore.
void Updates::vanished()
{
    ++m_ownerGeneration;
    forget();
    clearError();
    emit changed();
}

void Updates::forget()
{
    m_running = false;
    m_activity = QStringLiteral("idle");
    m_phase.clear();
    m_status = QStringLiteral("unknown");
    m_channel.clear();
    m_installedPackage.clear();
    m_installedVersion.clear();
    m_version.clear();
    m_tag.clear();
    m_commit.clear();
    m_changes.clear();
    m_changesTotal = 0;
    m_notes.clear();
    m_source.clear();
    m_lastCheck = 0;
    m_checkError.clear();
    m_checkMessage.clear();
    m_installResult.clear();
    m_installError.clear();
    m_installMessage.clear();
    m_installFinished = 0;
    m_needsRelogin = false;
    m_autoCheck = true;
    m_checkInterval = 24;
    m_nextCheck = 0;
    m_waitingForNetwork = false;
    m_announce.clear();
    m_autoCheckAsked.reported(m_autoCheck);
    m_checkIntervalAsked.reported(m_checkInterval);
}

void Updates::onPropertiesChanged(const QString& interface, const QVariantMap& changed, const QStringList&)
{
    if (interface == service) {
        setProperties(changed);
    }
}

void Updates::setProperties(const QVariantMap& values)
{
    const QHash<QString, QString*> strings {
        { QStringLiteral("Activity"), &m_activity },
        { QStringLiteral("Phase"), &m_phase },
        { QStringLiteral("Status"), &m_status },
        { QStringLiteral("Channel"), &m_channel },
        { QStringLiteral("InstalledPackage"), &m_installedPackage },
        { QStringLiteral("InstalledVersion"), &m_installedVersion },
        { QStringLiteral("Version"), &m_version },
        { QStringLiteral("Tag"), &m_tag },
        { QStringLiteral("Commit"), &m_commit },
        { QStringLiteral("Source"), &m_source },
        { QStringLiteral("InstallResult"), &m_installResult },
        { QStringLiteral("Announce"), &m_announce },
    };
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (QString* string = strings.value(it.key())) {
            *string = it->toString();
        } else if (it.key() == QLatin1String("Changes")) {
            m_changes = pairsOf(*it, QStringLiteral("commit"), QStringLiteral("summary"));
        } else if (it.key() == QLatin1String("ChangesTotal")) {
            m_changesTotal = int(it->toUInt());
        } else if (it.key() == QLatin1String("Notes")) {
            m_notes = pairsOf(*it, QStringLiteral("version"), QStringLiteral("text"));
        } else if (it.key() == QLatin1String("LastCheck")) {
            m_lastCheck = it->toLongLong();
        } else if (it.key() == QLatin1String("NextCheck")) {
            m_nextCheck = it->toLongLong();
        } else if (it.key() == QLatin1String("CheckInterval")) {
            m_checkInterval = it->toUInt();
        } else if (it.key() == QLatin1String("InstallFinished")) {
            m_installFinished = it->toLongLong();
        } else if (it.key() == QLatin1String("Error")) {
            std::tie(m_checkError, m_checkMessage) = pairOf(*it);
        } else if (it.key() == QLatin1String("InstallError")) {
            std::tie(m_installError, m_installMessage) = pairOf(*it);
        } else if (it.key() == QLatin1String("NeedsRelogin")) {
            m_needsRelogin = it->toBool();
        } else if (it.key() == QLatin1String("AutoCheck")) {
            m_autoCheck = it->toBool();
        } else if (it.key() == QLatin1String("WaitingForNetwork")) {
            m_waitingForNetwork = it->toBool();
        }
    }
    m_autoCheckAsked.reported(m_autoCheck);
    m_checkIntervalAsked.reported(m_checkInterval);
    emit changed();
}

} // namespace vela::updates
