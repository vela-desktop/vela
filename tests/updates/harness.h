// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the daemon's and the client library's tests run on (docs/updates.md
// §10): private session and system buses, a fake NetworkManager on the
// system one, a fake vela-update that writes its state files with the real
// vela-update-state, a fake terminal, and the daemon's test build, started
// and stopped per case. A test class derives from it and calls setUp(),
// reset(), release() and tearDown() from initTestCase(), init(), cleanup()
// and cleanupTestCase().

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

inline const QString service = QStringLiteral("org.vela.Updates");
inline const QString objectPath = QStringLiteral("/org/vela/Updates");
inline const QString commitA = QString(40, u'a');
inline const QString commitB = QString(40, u'b');
inline const QString networkManager = QStringLiteral("org.freedesktop.NetworkManager");

// NetworkManager's two properties the daemon reads (§6.2).
class FakeNetworkManager : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager")
    Q_PROPERTY(uint Metered READ metered)
    Q_PROPERTY(uint Connectivity READ connectivity)

public:
    uint metered() const { return m_metered; }
    uint connectivity() const { return m_connectivity; }

    void set(uint metered, uint connectivity)
    {
        m_metered = metered;
        m_connectivity = connectivity;
        QDBusMessage changed = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/NetworkManager"),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
        changed << QStringLiteral("org.freedesktop.NetworkManager")
                << QVariantMap { { QStringLiteral("Metered"), metered }, { QStringLiteral("Connectivity"), connectivity } }
                << QStringList {};
        QDBusConnection(QStringLiteral("fake-nm")).send(changed);
    }

private:
    uint m_metered = 2; // no
    uint m_connectivity = 4; // full
};

inline const char fakeUpdate[] = R"SH(#!/bin/sh
# The tests' vela-update: --check prints and records what check-output
# says; an install reports through the real vela-update-state.
printf '%s\n' "$*" >> "$FAKE/calls"
case " $* " in
*" --check "*)
    if [ -f "$FAKE/check-stubborn" ]; then trap '' TERM; fi
    # Like a stalled git fetch: a child that ignores TERM and holds stdout.
    if [ -f "$FAKE/check-orphan" ]; then
        (trap '' TERM; while :; do sleep 0.02; done) &
    fi
    while [ -f "$FAKE/check-hold" ]; do sleep 0.02; done
    if [ -f "$FAKE/check-output" ]; then
        cat "$FAKE/check-output"
        mkdir -p "$XDG_STATE_HOME/vela-update"
        cp "$FAKE/check-output" "$XDG_STATE_HOME/vela-update/.check.new"
        mv "$XDG_STATE_HOME/vela-update/.check.new" "$XDG_STATE_HOME/vela-update/check.json"
    fi
    exit "$(cat "$FAKE/check-exit" 2>/dev/null || echo 0)" ;;
esac
python3 -I "$STATE_HELPER" begin --pid $$ --action install --channel experimental
python3 -I "$STATE_HELPER" phase --pid $$ building
while [ ! -f "$FAKE/install-release" ]; do sleep 0.02; done
python3 -I "$STATE_HELPER" end --pid $$ --action install --channel experimental \
    --result "$(cat "$FAKE/install-result" 2>/dev/null || echo success)" --installed-version 0.1.1
)SH";

// A terminal that, like Konsole handing off to a running instance,
// returns at once; it runs the command only when terminal-runs exists.
inline const char fakeTerminal[] = R"SH(#!/bin/sh
# Whole or not at all: the tests wait for the file, then read it.
printf '%s\n' "$@" > "$FAKE/terminal.new"
mv "$FAKE/terminal.new" "$FAKE/terminal"
if [ -f "$FAKE/terminal-runs" ]; then
    "$@" < /dev/null > /dev/null 2>&1 &
fi
)SH";

class UpdatesBench {
public:
    // `connection`: the name of the test's own connection to the session
    // bus.
    explicit UpdatesBench(const QString& connection)
        : m_connection(connection)
    {
    }

protected:
    QProcess m_sessionBus;
    QProcess m_systemBus;
    QString m_sessionAddress;
    QString m_systemAddress;
    QTemporaryDir m_home;
    QProcess m_daemon;
    FakeNetworkManager m_network;
    int m_checks = 0; // checked_at grows with each check output

    QDBusConnection bus() const { return QDBusConnection(m_connection); }
    QString fake(const QString& name) const { return m_home.filePath(QStringLiteral("fake/") + name); }
    QString state(const QString& name) const { return m_home.filePath(QStringLiteral("state/vela-update/") + name); }

    static bool write(const QString& path, const QByteArray& content)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
    }

    static QByteArray read(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    // What the next check prints: schema 2 with `fields` over a default.
    void nextCheck(const QJsonObject& fields, int exitCode = 0)
    {
        QJsonObject check { { "schema_version", 2 }, { "channel", "experimental" }, { "status", "up-to-date" },
            { "installed_package", "vela-git" }, { "installed_version", "0.1.0.r1.gaaaaaaa-1" },
            { "version", "0.1.0" }, { "tag", "" }, { "commit", commitA }, { "source", "/src" },
            { "changes", QJsonArray {} }, { "changes_total", 0 }, { "notes", QJsonArray {} },
            { "checked_at", QDateTime::currentSecsSinceEpoch() - 1000 + ++m_checks }, { "error", QJsonValue::Null } };
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            check.insert(it.key(), it.value());
        }
        QVERIFY(write(fake("check-output"), QJsonDocument(check).toJson(QJsonDocument::Compact) + '\n'));
        QVERIFY(write(fake("check-exit"), QByteArray::number(exitCode)));
    }

    QStringList calls() const
    {
        return QString::fromUtf8(read(fake("calls"))).split(u'\n', Qt::SkipEmptyParts);
    }

    // The daemon's environment: the private buses, the home and the fakes.
    QProcessEnvironment daemonEnvironment(const QMap<QString, QString>& extra) const
    {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), m_sessionAddress);
        environment.insert(QStringLiteral("DBUS_SYSTEM_BUS_ADDRESS"), m_systemAddress);
        environment.insert(QStringLiteral("HOME"), m_home.path());
        environment.insert(QStringLiteral("XDG_STATE_HOME"), m_home.filePath(QStringLiteral("state")));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), m_home.filePath(QStringLiteral("config")));
        environment.insert(QStringLiteral("FAKE"), m_home.filePath(QStringLiteral("fake")));
        environment.insert(QStringLiteral("STATE_HELPER"), QStringLiteral(VELA_UPDATE_STATE));
        environment.insert(QStringLiteral("VELA_UPDATE_COMMAND"), fake(QStringLiteral("vela-update")));
        environment.insert(QStringLiteral("VELA_UPDATE_TERMINAL"), fake(QStringLiteral("terminal-program")));
        // No automatic check unless a case asks for one.
        environment.insert(QStringLiteral("VELA_UPDATE_FIRST_CHECK_MS"), QStringLiteral("3600000"));
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            environment.insert(it.key(), it.value());
        }
        return environment;
    }

    bool startDaemon(const QMap<QString, QString>& extra = {})
    {
        m_daemon.setProcessEnvironment(daemonEnvironment(extra));
        m_daemon.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        m_daemon.start(QStringLiteral(VELA_UPDATE_DAEMON), {});
        if (!m_daemon.waitForStarted()) {
            return false;
        }
        // Owned by this daemon: a stopped one's name can linger a moment.
        const auto owned = [this] { return bus().interface()->servicePid(service).value() == uint(m_daemon.processId()); };
        for (int i = 0; i < 500 && !owned(); ++i) {
            QTest::qWait(20);
        }
        if (!owned()) {
            qWarning("vela-update-daemon has no name: %s, exit %d",
                m_daemon.state() == QProcess::Running ? "running" : "not running", m_daemon.exitCode());
            return false;
        }
        return true;
    }

    void stopDaemon()
    {
        if (m_daemon.state() == QProcess::NotRunning) {
            return;
        }
        m_daemon.terminate();
        if (!m_daemon.waitForFinished(5000)) {
            m_daemon.kill();
            m_daemon.waitForFinished();
        }
        // Gone from the bus too, before the next case starts another.
        for (int i = 0; i < 250 && bus().interface()->isServiceRegistered(service); ++i) {
            QTest::qWait(20);
        }
    }

    // Empty, or what went wrong.
    QString setUp()
    {
        qDBusRegisterMetaType<QVariantMap>();
        for (auto [process, address] : { std::pair { &m_systemBus, &m_systemAddress }, std::pair { &m_sessionBus, &m_sessionAddress } }) {
            process->start(QStringLiteral("dbus-daemon"), { QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1") });
            if (!process->waitForStarted() || !process->waitForReadyRead()) {
                return QStringLiteral("dbus-daemon is needed");
            }
            *address = QString::fromUtf8(process->readAllStandardOutput().trimmed());
        }
        if (!QDBusConnection::connectToBus(m_sessionAddress, m_connection).isConnected()) {
            return QStringLiteral("no session bus connection");
        }
        QDBusConnection system = QDBusConnection::connectToBus(m_systemAddress, QStringLiteral("fake-nm"));
        if (!system.registerService(networkManager)
            || !system.registerObject(QStringLiteral("/org/freedesktop/NetworkManager"), &m_network,
                QDBusConnection::ExportAllProperties)) {
            return QStringLiteral("can't register the fake NetworkManager");
        }
        return {};
    }

    // Before each case: an empty home with the fakes, a free connection.
    void reset()
    {
        QDir(m_home.path()).removeRecursively();
        QDir().mkpath(m_home.filePath(QStringLiteral("config")));
        QVERIFY(write(fake(QStringLiteral("vela-update")), fakeUpdate));
        QVERIFY(write(fake(QStringLiteral("terminal-program")), fakeTerminal));
        for (const char* name : { "vela-update", "terminal-program" }) {
            QFile(fake(QLatin1String(name))).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        QDBusConnection(QStringLiteral("fake-nm")).registerService(networkManager);
        m_network.set(2, 4);
    }

    // After each case: lets any held fake go, and stops the daemon.
    void release()
    {
        QFile::remove(fake(QStringLiteral("check-hold")));
        write(fake(QStringLiteral("install-release")), "");
        stopDaemon();
    }

    void tearDown()
    {
        QDBusConnection::disconnectFromBus(m_connection);
        QDBusConnection::disconnectFromBus(QStringLiteral("fake-nm"));
        for (QProcess* process : { &m_systemBus, &m_sessionBus }) {
            process->kill();
            process->waitForFinished();
        }
    }

private:
    QString m_connection;
};
