// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-update-daemon on private buses (docs/updates.md §10): checks, their
// failures and timeouts, installs through a fake terminal, runs started by
// hand and killed, the announcement, automatic checks and NetworkManager,
// and "sign out to finish". vela-update is a fake that writes its state
// files with the real vela-update-state.

#include "harness.h"

#include <QDBusArgument>
#include <QDBusReply>
#include <QDBusVariant>

#include <csignal>
#include <sys/stat.h>

class UpdatesDaemon : public QObject, protected UpdatesBench {
    Q_OBJECT

public:
    UpdatesDaemon()
        : UpdatesBench(QStringLiteral("test"))
    {
    }

private:
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

    // Another daemon, run to its end: its exit code, or -1.
    int runDaemon(const QMap<QString, QString>& extra = {})
    {
        QProcess other;
        other.setProcessEnvironment(daemonEnvironment(extra));
        other.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        other.start(QStringLiteral(VELA_UPDATE_DAEMON), {});
        if (!other.waitForFinished(5000)) {
            other.kill();
            other.waitForFinished();
            return -1;
        }
        return other.exitStatus() == QProcess::NormalExit ? other.exitCode() : -1;
    }

    static QDBusConnection networkManagerBus() { return QDBusConnection(QStringLiteral("fake-nm")); }

private Q_SLOTS:
    void initTestCase()
    {
        const QString error = setUp();
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    void init() { reset(); }
    void cleanup() { release(); }
    void cleanupTestCase() { tearDown(); }

    void startsEmpty()
    {
        QVERIFY(startDaemon());
        QCOMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("unknown"));
        QCOMPARE(property(QStringLiteral("LastCheck")).toLongLong(), 0);
        QCOMPARE(property(QStringLiteral("InstallFinished")).toLongLong(), 0);
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

    // A check that ignores SIGTERM is killed.
    void stubbornCheckIsKilled()
    {
        nextCheck({});
        QVERIFY(write(fake(QStringLiteral("check-hold")), ""));
        QVERIFY(write(fake(QStringLiteral("check-stubborn")), ""));
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_CHECK_TIMEOUT_MS"), QStringLiteral("200") },
            { QStringLiteral("VELA_UPDATE_KILL_MS"), QStringLiteral("200") } }));
        call(QStringLiteral("Check"));
        QTest::qWait(300);
        QCOMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("checking"));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(property(QStringLiteral("Error")).toStringList().value(0), QStringLiteral("failed"));
    }

    // A child of the check that ignores SIGTERM and keeps stdout open (a
    // stalled git fetch) is killed with it: the check ends, the daemon is
    // idle again.
    void checkChildrenAreKilled()
    {
        nextCheck({});
        QVERIFY(write(fake(QStringLiteral("check-hold")), ""));
        QVERIFY(write(fake(QStringLiteral("check-orphan")), ""));
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_CHECK_TIMEOUT_MS"), QStringLiteral("200") },
            { QStringLiteral("VELA_UPDATE_KILL_MS"), QStringLiteral("200") } }));
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(property(QStringLiteral("Error")).toStringList().value(0), QStringLiteral("failed"));
    }

    // Megabytes on stdout aren't a report.
    void oversizedOutputIsNoReport()
    {
        QVERIFY(write(fake(QStringLiteral("check-output")), QByteArray(1100 * 1024, 'x')));
        QVERIFY(startDaemon());
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Error")).toStringList().value(0), QStringLiteral("failed"));
        QCOMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("unknown"));
    }

    void withoutVelaUpdate()
    {
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_COMMAND"), fake(QStringLiteral("missing")) } }));
        QCOMPARE(call(QStringLiteral("Check")).errorName(), QStringLiteral("org.vela.Updates.Error.Failed"));
        QCOMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
    }

    // None in PATH, or one that can't be run.
    void withoutTerminal()
    {
        QDir().mkpath(fake(QStringLiteral("empty")));
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_TERMINAL"), QString() },
            { QStringLiteral("PATH"), fake(QStringLiteral("empty")) }, { QStringLiteral("VELA_TERMINAL"), QString() } }));
        QCOMPARE(call(QStringLiteral("Install"), { QVariantMap {} }).errorName(),
            QStringLiteral("org.vela.Updates.Error.NoTerminal"));
        QCOMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        stopDaemon();
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_TERMINAL"), fake(QStringLiteral("missing")) } }));
        QCOMPARE(call(QStringLiteral("Install"), { QVariantMap {} }).errorName(),
            QStringLiteral("org.vela.Updates.Error.NoTerminal"));
        QCOMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
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
        // begin reports "resolving"; "building" follows a moment later.
        QTRY_COMPARE(property(QStringLiteral("Phase")).toString(), QStringLiteral("building"));
        QVERIFY(write(fake(QStringLiteral("install-result")), "failed"));
        QVERIFY(write(fake(QStringLiteral("install-release")), ""));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(property(QStringLiteral("InstallResult")).toString(), QStringLiteral("failed"));
        QCOMPARE(property(QStringLiteral("InstallError")).toStringList().value(0), QStringLiteral("failed"));
        QVERIFY(property(QStringLiteral("InstallFinished")).toLongLong() >= QDateTime::currentSecsSinceEpoch() - 60);
    }

    // The last check described the update just installed: checked again
    // once the run is gone; not after a failure.
    void successfulInstallChecksAgain()
    {
        QVERIFY(write(fake(QStringLiteral("terminal-runs")), ""));
        nextCheck({ { "status", "update-available" }, { "commit", commitB } });
        QVERIFY(startDaemon());
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("update-available"));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));

        QVERIFY(write(fake(QStringLiteral("install-result")), "failed"));
        QCOMPARE(call(QStringLiteral("Install"), { QVariantMap {} }).type(), QDBusMessage::ReplyMessage);
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("installing"));
        QVERIFY(write(fake(QStringLiteral("install-release")), ""));
        QTRY_COMPARE(property(QStringLiteral("InstallResult")).toString(), QStringLiteral("failed"));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QTest::qWait(300);
        QCOMPARE(calls().filter(QStringLiteral("--check")).size(), 1);

        QFile::remove(fake(QStringLiteral("install-release")));
        QVERIFY(write(fake(QStringLiteral("install-result")), "success"));
        nextCheck({});
        QCOMPARE(call(QStringLiteral("Install"), { QVariantMap {} }).type(), QDBusMessage::ReplyMessage);
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("installing"));
        QVERIFY(write(fake(QStringLiteral("install-release")), ""));
        QTRY_COMPARE(property(QStringLiteral("InstallResult")).toString(), QStringLiteral("success"));
        QTRY_COMPARE(property(QStringLiteral("Status")).toString(), QStringLiteral("up-to-date"));
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        QCOMPARE(calls().filter(QStringLiteral("--check")).size(), 2);
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

    void oneDaemonPerSession()
    {
        QVERIFY(startDaemon());
        QCOMPARE(runDaemon(), 2);
        QCOMPARE(m_daemon.state(), QProcess::Running);
    }

    // Where it can't keep its state, it doesn't start.
    void stateDirectoryUnusable()
    {
        QVERIFY(write(m_home.filePath(QStringLiteral("state")), "a file"));
        QCOMPARE(runDaemon(), 1);
        QVERIFY(!bus().interface()->isServiceRegistered(service));
    }

    // The source in updates.conf, or the default.
    void sourceFromUpdatesConf()
    {
        QVERIFY(write(m_home.filePath(QStringLiteral("config/vela/updates.conf")), "branch=main\nsource=/home/me/vela\n"));
        QVERIFY(startDaemon());
        QCOMPARE(property(QStringLiteral("Source")).toString(), QStringLiteral("/home/me/vela"));
        stopDaemon();
        QVERIFY(write(m_home.filePath(QStringLiteral("config/vela/updates.conf")), "source=\n"));
        QVERIFY(startDaemon());
        QCOMPARE(property(QStringLiteral("Source")).toString(), QStringLiteral("https://github.com/vela-desktop/vela.git"));
    }

    // A marker that can't be saved: announced for this session only.
    void announcedWithoutItsMarker()
    {
        nextCheck({ { "status", "update-available" }, { "commit", commitB } });
        QVERIFY(startDaemon());
        call(QStringLiteral("Check"));
        QTRY_COMPARE(property(QStringLiteral("Announce")).toString(), commitB);
        QTRY_COMPARE(property(QStringLiteral("Activity")).toString(), QStringLiteral("idle"));
        // In the way even for root, as CI runs.
        QVERIFY(QDir().mkpath(state(QStringLiteral("announced"))));
        QCOMPARE(call(QStringLiteral("Announced"), { commitB }).type(), QDBusMessage::ReplyMessage);
        QTRY_COMPARE(property(QStringLiteral("Announce")).toString(), QString());
        QVERIFY(QFileInfo(state(QStringLiteral("announced"))).isDir());
        QCOMPARE(QDir(state(QString())).entryList({ QStringLiteral(".announced.*") }, QDir::Files | QDir::Hidden), QStringList {});
        // The same again changes nothing; an empty one is no commit.
        QCOMPARE(call(QStringLiteral("Announced"), { commitB }).type(), QDBusMessage::ReplyMessage);
        QCOMPARE(call(QStringLiteral("Announced"), { QString() }).type(), QDBusMessage::ReplyMessage);
        QCOMPARE(property(QStringLiteral("Announce")).toString(), QString());
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

    // NetworkManager leaving while a check waits for it: nothing says the
    // connection is metered anymore. Coming back, it's read again.
    void networkManagerLeavesAndComesBack()
    {
        nextCheck({});
        m_network.set(1, 4); // metered
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_FIRST_CHECK_MS"), QStringLiteral("200") },
            { QStringLiteral("VELA_UPDATE_INTERVAL_MS"), QStringLiteral("1000") } }));
        QTest::qWait(700);
        QCOMPARE(calls(), QStringList {});
        QVERIFY(networkManagerBus().unregisterService(networkManager));
        QTRY_COMPARE(calls(), QStringList { QStringLiteral("--check --json") });

        // Back, still metered: the next automatic check waits for it again.
        QVERIFY(networkManagerBus().registerService(networkManager));
        QTest::qWait(1500);
        QCOMPARE(calls().size(), 1);
        m_network.set(2, 4);
        QTRY_COMPARE(calls().size(), 2);
    }

    // No NetworkManager at all, or no system bus: every connection may be
    // used.
    void withoutNetworkManager()
    {
        nextCheck({});
        QVERIFY(networkManagerBus().unregisterService(networkManager));
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_FIRST_CHECK_MS"), QStringLiteral("200") } }));
        QTRY_COMPARE(calls(), QStringList { QStringLiteral("--check --json") });
        stopDaemon();
        // Not due again after that check otherwise.
        QFile::remove(state(QStringLiteral("check.json")));
        QFile::remove(fake(QStringLiteral("calls")));
        QVERIFY(startDaemon({ { QStringLiteral("VELA_UPDATE_FIRST_CHECK_MS"), QStringLiteral("200") },
            { QStringLiteral("DBUS_SYSTEM_BUS_ADDRESS"), QStringLiteral("unix:path=") + fake(QStringLiteral("no-bus")) } }));
        QTRY_COMPARE(calls(), QStringList { QStringLiteral("--check --json") });
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
        // The "compositor": a copy of sh, waiting on its stdin, whose file
        // an update replaces. Not sleep: multicall coreutils (uutils on
        // Ubuntu) pick the tool from argv[0] and a renamed copy just exits.
        const QString copy = m_home.filePath(QStringLiteral("vela-compositor"));
        QVERIFY(QFile::copy(QStringLiteral("/bin/sh"), copy));
        QProcess compositor;
        compositor.start(copy, { QStringLiteral("-c"), QStringLiteral("read line") });
        QVERIFY(compositor.waitForStarted());
        QVERIFY(!compositor.waitForFinished(100));
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
