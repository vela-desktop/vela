// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screensharing.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScreen>
#include <QStandardPaths>

namespace {

QString sessionSocket()
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    return runtimeDir + QStringLiteral("/vela-") + display + QStringLiteral(".sock");
}

const QString fallbackIcon = QStringLiteral("video-display");

// An output as Settings > Display names it: make and model, else the
// compositor's description, else its connector.
QString outputLabel(const QJsonObject& target)
{
    const QString model = (target.value(QStringLiteral("make")).toString() + u' '
        + target.value(QStringLiteral("model")).toString())
                              .trimmed();
    if (!model.isEmpty()) {
        return model;
    }
    const QString description = target.value(QStringLiteral("description")).toString();
    return !description.isEmpty() ? description : target.value(QStringLiteral("name")).toString();
}

} // namespace

ScreenSharing::ScreenSharing(Resolver resolver, QObject* parent, const QString& socketPath, int graceMs)
    : QObject(parent)
    , m_resolver(std::move(resolver))
    , m_path(socketPath.isEmpty() ? sessionSocket() : socketPath)
{
    m_retry.setSingleShot(true);
    m_retry.setInterval(1000);
    connect(&m_retry, &QTimer::timeout, this, &ScreenSharing::connectToCompositor);
    m_grace.setSingleShot(true);
    m_grace.setInterval(graceMs);
    connect(&m_grace, &QTimer::timeout, this, [this] {
        if (!m_users.isEmpty()) {
            m_users.clear();
            emit changed();
        }
    });
    connect(&m_socket, &QLocalSocket::connected, this, [this] { m_socket.write("watch capture\n"); });
    connect(&m_socket, &QLocalSocket::readyRead, this, &ScreenSharing::read);
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        // Gone with the compositor: nothing it said holds any more.
        m_buffer.clear();
        m_state = {};
        update();
        m_retry.start();
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] {
        if (m_socket.state() == QLocalSocket::UnconnectedState) {
            m_retry.start();
        }
    });
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::screenAdded, this, &ScreenSharing::update);
        connect(app, &QGuiApplication::screenRemoved, this, &ScreenSharing::update);
    }
    connectToCompositor();
}

ScreenSharing::~ScreenSharing()
{
    // The socket closes after the rest of the object is gone: its
    // `disconnected` must not reach it.
    disconnect(&m_socket, nullptr, this, nullptr);
}

void ScreenSharing::connectToCompositor()
{
    if (m_socket.state() == QLocalSocket::UnconnectedState) {
        m_socket.connectToServer(m_path);
    }
}

void ScreenSharing::read()
{
    m_buffer += m_socket.readAll();
    // Every line is the whole state: only the newest matters.
    const qsizetype end = m_buffer.lastIndexOf('\n');
    if (end < 0) {
        return;
    }
    const qsizetype start = m_buffer.lastIndexOf('\n', end - 1) + 1;
    const QByteArray line = m_buffer.mid(start, end - start);
    m_buffer.remove(0, end + 1);
    apply(line);
}

void ScreenSharing::apply(const QByteArray& line)
{
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject() || !document.object().value(QStringLiteral("clients")).isArray()) {
        return;
    }
    m_state = document.object();
    update();
}

void ScreenSharing::setPortalApps(const QVariantList& apps)
{
    if (apps != m_portalApps) {
        m_portalApps = apps;
        update();
    }
}

void ScreenSharing::update()
{
    const auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    const int outputs = app ? int(QGuiApplication::screens().size()) : 1;
    const QVariantList users = describe(m_state, m_resolver, m_portalApps, outputs);
    if (!users.isEmpty()) {
        // A capture shows at once, never late.
        m_grace.stop();
        if (users != m_users) {
            m_users = users;
            emit changed();
        }
    } else if (!m_users.isEmpty() && !m_grace.isActive()) {
        m_grace.start(); // a second more: a portal reopening its session doesn't blink
    }
}

QVariantList ScreenSharing::describe(const QJsonObject& state, const Resolver& resolver,
    const QVariantList& portalApps, int outputs)
{
    QVariantList users;
    // The same app twice (through the portal and directly): one entry.
    auto add = [&users](QVariantMap user) {
        const QString key = user.value(QStringLiteral("desktopId")).toString();
        for (QVariant& existing : users) {
            QVariantMap entry = existing.toMap();
            const bool same = !key.isEmpty() ? entry.value(QStringLiteral("desktopId")).toString() == key
                                             : entry.value(QStringLiteral("name")) == user.value(QStringLiteral("name"));
            if (!same) {
                continue;
            }
            QStringList what = entry.value(QStringLiteral("targets")).toStringList();
            for (const QString& target : user.value(QStringLiteral("targets")).toStringList()) {
                if (!what.contains(target)) {
                    what.append(target);
                }
            }
            entry[QStringLiteral("targets")] = what;
            entry[QStringLiteral("what")] = what.join(QStringLiteral(", "));
            entry[QStringLiteral("screen")] = entry.value(QStringLiteral("screen")).toBool()
                || user.value(QStringLiteral("screen")).toBool();
            if (entry.value(QStringLiteral("window")).toString().isEmpty()) {
                entry[QStringLiteral("window")] = user.value(QStringLiteral("window"));
            }
            existing = entry;
            return;
        }
        users.append(user);
    };

    for (const QJsonValue& value : state.value(QStringLiteral("clients")).toArray()) {
        const QJsonObject client = value.toObject();
        QStringList what;
        bool screen = false;
        for (const QJsonValue& t : client.value(QStringLiteral("targets")).toArray()) {
            const QJsonObject target = t.toObject();
            QString text;
            if (target.value(QStringLiteral("type")).toString() == QLatin1String("output")) {
                screen = true;
                text = outputs <= 1 ? tr("Your screen") : outputLabel(target);
            } else {
                const QString appId = target.value(QStringLiteral("app_id")).toString();
                const App owner = appId.isEmpty() ? App {} : resolver(QStringList { appId });
                const QString name = !owner.name.isEmpty() ? owner.name : appId;
                text = name.isEmpty() ? tr("A window") : tr("A window of %1").arg(name);
            }
            if (!what.contains(text)) {
                what.append(text);
            }
        }
        if (what.isEmpty()) {
            continue;
        }
        QVariantMap user {
            { QStringLiteral("targets"), what },
            { QStringLiteral("what"), what.join(QStringLiteral(", ")) },
            { QStringLiteral("screen"), screen },
        };
        if (client.value(QStringLiteral("portal")).toBool()) {
            // The apps receiving the portal's streams (§5.2).
            if (portalApps.isEmpty()) {
                user[QStringLiteral("name")] = tr("An app");
                user[QStringLiteral("icon")] = fallbackIcon;
                user[QStringLiteral("desktopId")] = QString();
                user[QStringLiteral("window")] = QString();
                add(user);
            }
            for (const QVariant& portalApp : portalApps) {
                const QVariantMap a = portalApp.toMap();
                const QString icon = a.value(QStringLiteral("icon")).toString();
                user[QStringLiteral("name")] = a.value(QStringLiteral("name"));
                user[QStringLiteral("icon")] = icon.isEmpty() ? fallbackIcon : icon;
                user[QStringLiteral("desktopId")] = a.value(QStringLiteral("desktopId")).toString();
                user[QStringLiteral("window")] = QString();
                add(user);
            }
            continue;
        }
        const QString appId = client.value(QStringLiteral("app_id")).toString();
        const QString binary = client.value(QStringLiteral("binary")).toString();
        QStringList candidates;
        for (const QString& candidate : { appId, binary }) {
            if (!candidate.isEmpty()) {
                candidates.append(candidate);
            }
        }
        const App app = candidates.isEmpty() ? App {} : resolver(candidates);
        user[QStringLiteral("name")] = !app.name.isEmpty() ? app.name : !binary.isEmpty() ? binary : tr("An app");
        user[QStringLiteral("icon")] = app.icon.isEmpty() ? fallbackIcon : app.icon;
        user[QStringLiteral("desktopId")] = app.desktopId;
        user[QStringLiteral("window")] = client.value(QStringLiteral("window")).toString();
        add(user);
    }
    return users;
}

QString ScreenSharing::summarize(const QVariantList& users)
{
    if (users.isEmpty()) {
        return {};
    }
    if (users.size() == 1) {
        const QVariantMap user = users.first().toMap();
        const QString name = user.value(QStringLiteral("name")).toString();
        return user.value(QStringLiteral("screen")).toBool() ? tr("%1 is sharing your screen").arg(name)
                                                             : tr("%1 is sharing a window").arg(name);
    }
    QStringList lines { tr("These apps are sharing your screen:") };
    for (const QVariant& user : users) {
        lines.append(user.toMap().value(QStringLiteral("name")).toString());
    }
    return lines.join(u'\n');
}
