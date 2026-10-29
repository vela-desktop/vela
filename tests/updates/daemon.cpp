// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-update-daemon on private buses (docs/updates.md §10): checks, their
// failures and timeouts, installs through a fake terminal, runs started by
// hand and killed, the announcement, automatic checks and NetworkManager,
// and "sign out to finish". vela-update is a fake that writes its state
// files with the real vela-update-state.

#include <QJsonArray>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <sys/stat.h>

namespace {

const QString service = QStringLiteral("org.vela.Updates");
const QString objectPath = QStringLiteral("/org/vela/Updates");
const QString commitA = QString(40, u'a');
const QString commitB = QString(40, u'b');

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

const char fakeUpdate[] = R"SH(#!/bin/sh
# The tests' vela-update: --check prints and records what check-output
# says; an install reports through the real vela-update-state.
printf '%s\n' "$*" >> "$FAKE/calls"
case " $* " in
*" --check "*)
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
const char fakeTerminal[] = R"SH(#!/bin/sh
printf '%s\n' "$@" > "$FAKE/terminal"
if [ -f "$FAKE/terminal-runs" ]; then
    "$@" < /dev/null > /dev/null 2>&1 &
fi
)SH";

} // namespace

class UpdatesDaemon : public QObject {
    Q_OBJECT

    QProcess m_sessionBus;
    QProcess m_systemBus;
    QString m_sessionAddress;
    QString m_systemAddress;
    QTemporaryDir m_home;
    QProcess m_daemon;
    FakeNetworkManager m_network;
    int m_checks = 0; // checked_at grows with each check output

    QDBusConnection bus() const { return QDBusConnection(QStringLiteral("test")); }
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

    QVariant property(const QString& name)
    {
        QDBusMessage get = QDBusMessage::createMethodCall(service, objectPath,
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        get << service << name;
        QDBusMessage reply = bus().call(get, QDBus::Block, 5000);
        if (reply.type() != QDBusMessage::ReplyMessage) {
            return {};
        }
        QVariant value = reply.arguments().value(0).value<QDBusVariant>().variant();
        if (value.canConvert<QDBusArgument>()) {
            // (ss) and a(ss), as lists of strings.
            const QDBusArgument argument = value.value<QDBusArgument>();
            QStringList strings;
            if (argument.currentType() == QDBusArgument::StructureType) {
                QString first, second;
                argument.beginStructure();
                argument >> first >> second;
                argument.endStructure();
                return QStringList { first, second };
            }
            argument.beginArray();
            while (!argument.atEnd()) {
                QString first, second;
                argument.beginStructure();
                argument >> first >> second;
                argument.endStructure();
                strings << first << second;
            }
            argument.endArray();
            return strings;
        }
        return value;
    }

    QDBusMessage call(const QString& method, const QVariantList& arguments = {})
    {
        QDBusMessage message = QDBusMessage::createMethodCall(service, objectPath, service, method);
        message.setArguments(arguments);
        return bus().call(message, QDBus::Block, 5000);
    }

    QStringList calls() const
    {
        return QString::fromUtf8(read(fake("calls"))).split(u'\n', Qt::SkipEmptyParts);
    }

    bool startDaemon(const QMap<QString, QString>& extra = {})
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
        m_daemon.setProcessEnvironment(environment);
        m_daemon.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        m_daemon.start(QStringLiteral(VELA_UPDATE_DAEMON), {});
        if (!m_daemon.waitForStarted()) {
            return false;
        }
        for (int i = 0; i < 500 && !bus().interface()->isServiceRegistered(service); ++i) {
            QTest::qWait(20);
        }
        return bus().interface()->isServiceRegistered(service);
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
    }

private Q_SLOTS:
    void initTestCase()
    {
        qDBusRegisterMetaType<QVariantMap>();
        for (auto [process, address] : { std::pair { &m_systemBus, &m_systemAddress }, std::pair { &m_sessionBus, &m_sessionAddress } }) {
            process->start(QStringLiteral("dbus-daemon"), { QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1") });
            if (!process->waitForStarted() || !process->waitForReadyRead()) {
                QSKIP("dbus-daemon is needed");
            }
            *address = QString::fromUtf8(process->readAllStandardOutput().trimmed());
        }
        QVERIFY(QDBusConnection::connectToBus(m_sessionAddress, QStringLiteral("test")).isConnected());
        QDBusConnection system = QDBusConnection::connectToBus(m_systemAddress, QStringLiteral("fake-nm"));
        QVERIFY(system.registerService(QStringLiteral("org.freedesktop.NetworkManager")));
        QVERIFY(system.registerObject(QStringLiteral("/org/freedesktop/NetworkManager"), &m_network,
            QDBusConnection::ExportAllProperties));
    }

    void init()
    {
        QDir(m_home.path()).removeRecursively();
        QDir().mkpath(m_home.filePath(QStringLiteral("config")));
        QVERIFY(write(fake(QStringLiteral("vela-update")), fakeUpdate));
        QVERIFY(write(fake(QStringLiteral("terminal-program")), fakeTerminal));
        for (const char* name : { "vela-update", "terminal-program" }) {
            QFile(fake(QLatin1String(name))).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        m_network.set(2, 4);
    }

    void cleanup()
    {
        // Lets any held fake go.
        QFile::remove(fake(QStringLiteral("check-hold")));
        write(fake(QStringLiteral("install-release")), "");
        stopDaemon();
    }

    void cleanupTestCase()
    {
        QDBusConnection::disconnectFromBus(QStringLiteral("test"));
        QDBusConnection::disconnectFromBus(QStringLiteral("fake-nm"));
        for (QProcess* process : { &m_systemBus, &m_sessionBus }) {
            process->kill();
            process->waitForFinished();
        }
    }

    void startsEmpty()
    {
        QVERIFY(startDaemon());
        QCOMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("unknown"));
        QCOMPARE(property(QStringLiteral("LastCheck")).toLongLong(), 0);
        QCOMPARE(property(QStringLiteral("AutoCheck")).toBool(), true);
        QCOMPARE(property(QStringLiteral("NeedsRelogin")).toBool(), false);
        QCOMPARE(property(QStringLiteral("Source")).toString(), QStringLiteral("https://github.com/vela-desktop/vela.git"));
        QCOMPARE(property(QStringLiteral("Error")).toStringList(), (QStringList { QString(), QString() }));
        QCOMPARE(property(QStringLiteral("Announce")).toString(), QString());
    }

    void checkFindsAnUpdate()
    {
        nextCheck({ { "status", "update-available" }, { "commit", commitB },
            { "changes", QJsonArray { QJsonObject { { "commit", commitB }, { "summary", "Faster blur" } } } },
            { "changes_total", 1 } });
        QVERIFY(startDaemon());
        QCOMPARE(call(QStringLiteral("Check")).type(), QDBusMessage::ReplyMessage);
        QTRY_COMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("update-available"));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(calls(), QStringList { QStringLiteral("--check --json") });
        QCOMPARE(property(QStringLiteral("Commit")).toString(), commitB);
        QCOMPARE(property(QStringLiteral("Changes")).toStringList(), (QStringList { commitB, QStringLiteral("Faster blur") }));
        QCOMPARE(property(QStringLiteral("ChangesTotal")).toUInt(), 1u);
        QVERIFY(property(QStringLiteral("LastCheck")).toLongLong() > QDateTime::currentSecsSinceEpoch() - 1100);
        QCOMPARE(property(QStringLiteral("InstalledPackage")).toString(), QStringLiteral("vela-git"));

        // Restarted, it still knows (check.json).
        stopDaemon();
        QVERIFY(startDaemon());
        QCOMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("update-available"));
    }

    void observesExternalChecksWithinOneSecond()
    {
        const qint64 second = QDateTime::currentSecsSinceEpoch() - 1000;
        nextCheck({ { "checked_at", second } });
        QVERIFY(startDaemon());
        QCOMPARE(call(QStringLiteral("Check")).type(), QDBusMessage::ReplyMessage);
        QTRY_COMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("up-to-date"));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));

        // A CLI check in another process atomically replaces check.json,
        // but checked_at is exactly the same second as the previous check.
        nextCheck({ { "checked_at", second }, { "status", "update-available" }, { "commit", commitB } });
        QProcess external;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("FAKE"), m_home.filePath(QStringLiteral("fake")));
        environment.insert(QStringLiteral("XDG_STATE_HOME"), m_home.filePath(QStringLiteral("state")));
        external.setProcessEnvironment(environment);
        external.start(fake(QStringLiteral("vela-update")), { QStringLiteral("--check") });
        QVERIFY(external.waitForStarted());
        QVERIFY(external.waitForFinished(5000));
        QCOMPARE(external.exitCode(), 0);
        QTRY_COMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("update-available"));
        QCOMPARE(property(QStringLiteral("Commit")).toString(), commitB);
    }

    void oneCheckAtATime()
    {
        nextCheck({});
        QVERIFY(write(fake(QStringLiteral("check-hold")), ""));
        QVERIFY(startDaemon());
        QCOMPARE(call(QStringLiteral("Check")).type(), QDBusMessage::ReplyMessage);
        QCOMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("checking"));
        QDBusMessage busy = call(QStringLiteral("Check"));
        QCOMPARE(busy.errorName(), QStringLiteral("org.vela.Updates.Error.Busy"));
        QCOMPARE(call(QStringLiteral("Install"), { QVariantMap {} }).errorName(),
            QStringLiteral("org.vela.Updates.Error.Busy"));
        QFile::remove(fake(QStringLiteral("check-hold")));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("up-to-date"));
    }

    void failureKeepsTheLastCheck()
    {
        nextCheck({ { "status", "update-available" }, { "commit", commitB } });
        QVERIFY(startDaemon());
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("update-available"));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));

        nextCheck({ { "status", "error" }, { "error", QJsonObject { { "code", "rate-limited" }, { "message", "HTTP 403" } } } }, 1);
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Error")).toStringList(),
            (QStringList { QStringLiteral("rate-limited"), QStringLiteral("HTTP 403") }));
        QCOMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("update-available"));
        QCOMPARE(property(QStringLiteral("Commit")).toString(), commitB);
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));

        // No report at all.
        QFile::remove(fake(QStringLiteral("check-output")));
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Error")).toStringList().value(0), QStringLiteral("failed"));

        // A completed check clears the failure.
        nextCheck({});
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("up-to-date"));
        QCOMPARE(property(QStringLiteral("Error")).toStringList(), (QStringList { QString(), QString() }));
    }

    void slowCheckIsStopped()
    {
        nextCheck({});
        QVERIFY(write(fake(QStringLiteral("check-hold")), ""));
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_CHECK_TIMEOUT_MS"), QStringLiteral("300") } }));
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(property(QStringLiteral("Error")).toStringList().value(0), QStringLiteral("failed"));
    }

    void installRunsInTheTerminal()
    {
        QVERIFY(write(fake(QStringLiteral("terminal-runs")), ""));
        QVERIFY(startDaemon());
        QCOMPARE(call(QStringLiteral("Install"), { QVariantMap { { QStringLiteral("channel"), QStringLiteral("experimental") },
                                                     { QStringLiteral("branch"), QStringLiteral("fix/nvidia") } } })
                     .type(),
            QDBusMessage::ReplyMessage);
        // sh -c '<the command, then a pause>' sh vela-update <options>
        QTRY_VERIFY(QFile::exists(fake(QStringLiteral("terminal"))));
        QStringList terminal = QString::fromUtf8(read(fake(QStringLiteral("terminal")))).split(u'\n', Qt::SkipEmptyParts);
        QCOMPARE(terminal.value(0), QStringLiteral("sh"));
        QCOMPARE(terminal.value(1), QStringLiteral("-c"));
        QVERIFY(terminal.value(2).contains(QStringLiteral("Press Enter")));
        QCOMPARE(terminal.mid(3, 6), (QStringList { QStringLiteral("sh"), fake(QStringLiteral("vela-update")),
                                          QStringLiteral("--channel"), QStringLiteral("experimental"),
                                          QStringLiteral("--branch"), QStringLiteral("fix/nvidia") }));

        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("installing"));
        QCOMPARE(property(QStringLiteral("Phase")).toString(), QStringLiteral("building"));
        QVERIFY(write(fake(QStringLiteral("install-result")), "failed"));
        QVERIFY(write(fake(QStringLiteral("install-release")), ""));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(property(QStringLiteral("InstallResult")).toString(), QStringLiteral("failed"));
        QCOMPARE(property(QStringLiteral("InstallError")).toStringList().value(0), QStringLiteral("failed"));
    }

    void terminalThatNeverStarts()
    {
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_START_TIMEOUT_MS"), QStringLiteral("300") } }));
        QCOMPARE(call(QStringLiteral("Install"), { QVariantMap {} }).type(), QDBusMessage::ReplyMessage);
        QCOMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("starting"));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(property(QStringLiteral("InstallResult")).toString(), QStringLiteral("failed"));
        QCOMPARE(property(QStringLiteral("InstallError")).toStringList().value(0), QStringLiteral("terminal"));
    }

    void invalidOptions()
    {
        QVERIFY(startDaemon());
        const QList<QVariantMap> invalid = {
            { { QStringLiteral("channel"), QStringLiteral("stable") } },
            { { QStringLiteral("branch"), QStringLiteral("a..b") } },
            { { QStringLiteral("branch"), QStringLiteral("main") }, { QStringLiteral("commit"), QStringLiteral("abcd") } },
            { { QStringLiteral("channel"), QStringLiteral("beta") }, { QStringLiteral("source"), QStringLiteral("/x") } },
            { { QStringLiteral("unknown"), QStringLiteral("x") } },
            { { QStringLiteral("branch"), 7 } },
        };
        for (const QVariantMap& options : invalid) {
            QCOMPARE(call(QStringLiteral("Install"), { options }).errorName(),
                QStringLiteral("org.vela.Updates.Error.InvalidArgs"));
        }
        QVERIFY(!QFile::exists(fake(QStringLiteral("terminal"))));
        QCOMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
    }

    void runByHandAndKilled()
    {
        QVERIFY(startDaemon());
        // An update started from a terminal by hand.
        QProcess byHand;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("FAKE"), m_home.filePath(QStringLiteral("fake")));
        environment.insert(QStringLiteral("STATE_HELPER"), QStringLiteral(VELA_UPDATE_STATE));
        environment.insert(QStringLiteral("XDG_STATE_HOME"), m_home.filePath(QStringLiteral("state")));
        byHand.setProcessEnvironment(environment);
        byHand.start(fake(QStringLiteral("vela-update")), {});
        QVERIFY(byHand.waitForStarted());
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("installing"));
        QCOMPARE(call(QStringLiteral("Check")).errorName(), QStringLiteral("org.vela.Updates.Error.Busy"));
        // Killed without its exit trap: run.json stays, the daemon notices.
        ::kill(pid_t(byHand.processId()), SIGKILL);
        byHand.waitForFinished();
        QVERIFY(QFile::exists(state(QStringLiteral("run.json"))));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(property(QStringLiteral("InstallResult")).toString(), QStringLiteral("cancelled"));
    }

    void announcedOncePerUpdate()
    {
        nextCheck({ { "status", "update-available" }, { "commit", commitB } });
        QVERIFY(startDaemon());
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Announce")).toString(), commitB);
        QCOMPARE(call(QStringLiteral("Announced"), { commitB }).type(), QDBusMessage::ReplyMessage);
        QTRY_COMPARE(property(QStringLiteral("Announce")).toString(), QString());
        // A notification marker is private even with a permissive umask.
        struct stat marker {};
        const QByteArray markerPath = QFile::encodeName(state(QStringLiteral("announced")));
        QVERIFY(::stat(markerPath.constData(), &marker) == 0);
        QCOMPARE(marker.st_mode & 0777, mode_t(0600));
        // Across sessions too.
        stopDaemon();
        QVERIFY(startDaemon());
        QCOMPARE(property(QStringLiteral("Announce")).toString(), QString());
        // A newer update is announced again; being up to date is not news.
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        nextCheck({ { "status", "update-available" }, { "commit", commitA } });
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Announce")).toString(), commitA);
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        nextCheck({});
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("up-to-date"));
        QCOMPARE(property(QStringLiteral("Announce")).toString(), QString());
    }

    void autoCheckSetting()
    {
        QVERIFY(startDaemon());
        QCOMPARE(call(QStringLiteral("SetAutoCheck"), { false }).type(), QDBusMessage::ReplyMessage);
        QTRY_COMPARE(property(QStringLiteral("AutoCheck")).toBool(), false);
        QVERIFY(read(m_home.filePath(QStringLiteral("config/vela/vela.conf"))).contains("update-check=no"));
        stopDaemon();
        QVERIFY(startDaemon());
        QCOMPARE(property(QStringLiteral("AutoCheck")).toBool(), false);
    }

    void automaticChecksWaitForAFreeConnection()
    {
        nextCheck({});
        m_network.set(1, 4); // metered
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_FIRST_CHECK_MS"), QStringLiteral("200") } }));
        QTest::qWait(700);
        QCOMPARE(calls(), QStringList {});
        m_network.set(2, 4);
        QTRY_COMPARE(calls(), QStringList { QStringLiteral("--check --json") });

        // Off: none.
        stopDaemon();
        QFile::remove(fake(QStringLiteral("calls")));
        write(m_home.filePath(QStringLiteral("config/vela/vela.conf")), "update-check=no\n");
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_FIRST_CHECK_MS"), QStringLiteral("200") },
            { QStringLiteral("VELA_UPDATE_INTERVAL_MS"), QStringLiteral("200") } }));
        QTest::qWait(700);
        QCOMPARE(calls(), QStringList {});
    }

    void automaticChecksRequireFullConnectivity()
    {
        nextCheck({});
        m_network.set(2, 0); // unknown, even if the network may actually work
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_FIRST_CHECK_MS"), QStringLiteral("200") } }));
        QTest::qWait(700);
        QCOMPARE(calls(), QStringList {});
        m_network.set(2, 3); // limited / captive portal
        QTest::qWait(400);
        QCOMPARE(calls(), QStringList {});
        m_network.set(2, 4); // full
        QTRY_COMPARE(calls(), (QStringList { QStringLiteral("--check --json") }));
    }

    void networkErrorsAreRetried()
    {
        nextCheck({ { "status", "error" }, { "error", QJsonObject { { "code", "network" }, { "message", "offline" } } } }, 1);
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_FIRST_CHECK_MS"), QStringLiteral("100") },
            { QStringLiteral("VELA_UPDATE_RETRY_MS"), QStringLiteral("300") } }));
        QTRY_COMPARE(calls().size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(calls().size(), 2, 5000);
        // Doubling: 300 ms, then 600 ms; never a burst.
        QTest::qWait(300);
        QCOMPARE(calls().size(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(calls().size(), 3, 5000);
    }

    void signOutAfterTheCompositorIsReplaced()
    {
        // The "compositor": a copy of sleep whose file an update replaces.
        const QString copy = m_home.filePath(QStringLiteral("vela-compositor"));
        QVERIFY(QFile::copy(QStringLiteral("/usr/bin/sleep"), copy));
        QProcess compositor;
        compositor.start(copy, { QStringLiteral("60") });
        QVERIFY(compositor.waitForStarted());
        nextCheck({});
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_SESSION_PID"), QString::number(compositor.processId()) } }));
        QCOMPARE(property(QStringLiteral("NeedsRelogin")).toBool(), false);
        QVERIFY(QFile::remove(copy));
        // Noticed at the next change of the state files.
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("NeedsRelogin")).toBool(), true);
        compositor.kill();
        compositor.waitForFinished();
    }
};

QTEST_GUILESS_MAIN(UpdatesDaemon)
#include "daemon.moc"
