// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Real QProcess clients talking to an isolated, controllably slow service:
// the network (nmcli). No running NetworkManager is touched. (Sound has
// its own tests on a private PipeWire, tests/audio.)
#include "asyncprocess.h"
#include "network.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <cerrno>
#include <memory>
#include <unistd.h>

namespace {

QString serviceRoot() { return qEnvironmentVariable("VELA_TEST_SERVICES"); }

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void saveFile(const QString& path, const QByteArray& data)
{
    QSaveFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
    QVERIFY(file.commit());
}

QJsonObject configuration()
{
    return QJsonDocument::fromJson(readFile(serviceRoot() + "/config.json")).object();
}

void logRequest(const QString& operation, const QStringList& arguments = {})
{
    const QJsonObject record { { "operation", operation }, { "arguments", QJsonArray::fromStringList(arguments) },
        { "pid", qint64(::getpid()) } };
    QFile log(serviceRoot() + "/requests.jsonl");
    if (log.open(QIODevice::WriteOnly | QIODevice::Append)) {
        log.write(QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n');
    }
}

QString operation(const QStringList& args)
{
    if (args.contains("show")) {
        return "details";
    }
    if (args.contains("connect") || (args.value(0) == "connection" && args.value(1) == "up")) {
        return "connect";
    }
    if (args.contains("disconnect") || args.contains("down") || args.contains("delete")) {
        return "action";
    }
    if (args.last() == "device") {
        return "devices";
    }
    if (args.last() == "connection") {
        return "known";
    }
    return args.last() == "yes" ? "scan" : "wifi";
}

int fakeService(const QStringList& arguments)
{
    const QString key = operation(arguments);
    // Capture the response before waiting: it can become obsolete in flight.
    const QJsonObject response = configuration().value(key).toObject();
    const QByteArray output = response.value("output").toString().toUtf8();
    logRequest(key, arguments);
    const QString gate = response.value("gate").toString();
    while (!gate.isEmpty() && !QFileInfo::exists(serviceRoot() + '/' + gate)) {
        QThread::msleep(5); // deliberately slow service, in a separate process
    }
    QThread::msleep(response.value("delay").toInt());
    ::write(STDOUT_FILENO, output.constData(), output.size());
    const QByteArray error = response.value("error").toString().toUtf8();
    ::write(STDERR_FILENO, error.constData(), error.size());
    return response.value("code").toInt();
}

} // namespace

class ServiceModels : public QObject {
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<Network> m_model;
    QJsonObject m_config;
    QByteArray m_path;
    QProcess m_bus;

    void save() { saveFile(m_dir->filePath("config.json"), QJsonDocument(m_config).toJson()); }
    void response(const QString& key, const QByteArray& output)
    {
        m_config.insert(key, QJsonObject { { "output", QString::fromUtf8(output) } });
        save();
    }
    void option(const QString& key, const QString& name, const QJsonValue& value)
    {
        QJsonObject response = m_config.value(key).toObject();
        response.insert(name, value);
        m_config.insert(key, response);
        save();
    }
    QList<QJsonObject> requests() const
    {
        QList<QJsonObject> records;
        for (const QByteArray& line : readFile(m_dir->filePath("requests.jsonl")).split('\n')) {
            if (!line.isEmpty()) {
                records.append(QJsonDocument::fromJson(line).object());
            }
        }
        return records;
    }
    int count(const QString& operation) const
    {
        int result = 0;
        for (const QJsonObject& record : requests()) {
            result += record.value("operation").toString() == operation;
        }
        return result;
    }
    bool clientsStopped() const
    {
        for (const QJsonObject& record : requests()) {
            if (::kill(record.value("pid").toInt(), 0) == 0) {
                return false;
            }
        }
        return true;
    }
    void makeModel()
    {
        m_model = std::make_unique<Network>();
        QVERIFY(m_model->property("available").toBool());
    }
    void refresh() { m_model->refresh(); }
    QVariantMap cache() const
    {
        QVariantMap result;
        for (const QString property : { "devices", "wifiNetworks" }) {
            result.insert(property, m_model->property(property.toLatin1().constData()));
        }
        return result;
    }
    bool populated() const
    {
        for (const QVariant& value : cache()) {
            if (value.toList().isEmpty()) {
                return false;
            }
        }
        return true;
    }
    void gateReads()
    {
        for (const QString key : { "devices", "details", "known", "wifi" }) {
            option(key, "gate", "release");
        }
    }

private slots:
    void initTestCase()
    {
        // Map Qt's system-bus connection to a private bus, never the real one.
        m_bus.start("dbus-daemon", { "--session", "--nofork", "--print-address=1" });
        QVERIFY(m_bus.waitForStarted());
        QVERIFY2(m_bus.waitForReadyRead(), m_bus.readAllStandardError().constData());
        qputenv("DBUS_SYSTEM_BUS_ADDRESS", m_bus.readAllStandardOutput().trimmed());
    }
    void cleanupTestCase()
    {
        if (m_bus.state() != QProcess::NotRunning) {
            m_bus.kill();
            QVERIFY(m_bus.waitForFinished());
        }
    }
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        m_path = qgetenv("PATH");
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), m_dir->filePath("nmcli")));
        qputenv("PATH", QFile::encodeName(m_dir->path()));
        qputenv("VELA_TEST_SERVICES", QFile::encodeName(m_dir->path()));
        m_config = {};
        response("devices", "eth0:ethernet:connected:Wired\nwlan0:wifi:connected:Home\\:Lab\nlo:loopback:connected:lo\n");
        response("details", "GENERAL.HWADDR:AA\\:BB\nIP4.ADDRESS[1]:192.0.2.2/24\nIP4.GATEWAY:192.0.2.1\nIP4.DNS[1]:192.0.2.53\nIP6.ADDRESS[1]:2001\\:db8\\:\\:1/64\n");
        response("known", "Home\\:Lab:802-11-wireless\n");
        response("wifi", "*:Home\\:Lab:70:WPA2\n:Other:90:--\n");
        response("scan", "*:Home\\:Lab:70:WPA2\n:Other:90:--\n");
    }
    void cleanup()
    {
        m_model.reset();
        QTRY_VERIFY(clientsStopped());
        qputenv("PATH", m_path);
        qunsetenv("VELA_TEST_SERVICES");
        m_dir.reset();
    }

    void delayedRefreshKeepsCacheAndEventLoop()
    {
        makeModel();
        refresh();
        QTRY_VERIFY(populated());
        const QVariantMap before = cache();
        saveFile(m_dir->filePath("requests.jsonl"), {});
        gateReads();
        QElapsedTimer elapsed;
        elapsed.start();
        refresh();
        QVERIFY2(elapsed.elapsed() < 250, "opening the panel must not wait for services");
        QCOMPARE(cache(), before);

        int beats = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; });
        heartbeat.start(5);
        QTRY_VERIFY(beats >= 10);
        QCOMPARE(cache(), before);
        for (int i = 0; i < 20; ++i) {
            refresh(); // bursts become one pending refresh, not twenty
        }
        QCOMPARE(count("devices"), 1);
        saveFile(m_dir->filePath("release"), {});
        QTRY_COMPARE(count("devices"), 2);
        QTRY_VERIFY(populated());
        QCOMPARE(cache(), before);
        QCOMPARE(m_model->devices().size(), 2);
        QCOMPARE(m_model->devices().first().toMap().value("mac").toString(), QString("AA:BB"));
        QCOMPARE(m_model->wifiNetworks().first().toMap().value("ssid").toString(), QString("Home:Lab"));
        QVERIFY(m_model->wifiNetworks().first().toMap().value("known").toBool());
    }

    void failedRefreshKeepsCache_data()
    {
        QTest::addColumn<QString>("failure");
        for (const QString failure : { "exit", "invalid", "timeout" }) {
            QTest::newRow(failure.toLatin1().constData()) << failure;
        }
    }
    void failedRefreshKeepsCache()
    {
        QFETCH(QString, failure);
        makeModel();
        refresh();
        QTRY_VERIFY(populated());
        const QVariantMap before = cache();
        const QStringList keys { "devices", "wifi" };
        for (const QString& key : keys) {
            if (failure == "exit") {
                option(key, "code", 3);
                option(key, "output", "partial response");
            } else if (failure == "invalid") {
                option(key, "output", "not a valid response");
            } else {
                option(key, "gate", "never-release");
            }
        }
        int beats = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; });
        heartbeat.start(10);
        refresh();
        QTest::qWait(failure == "timeout" ? 3300 : 150);
        QVERIFY(beats >= 5);
        QCOMPARE(cache(), before);
        // Failed/time-limited requests release their busy state and can retry.
        saveFile(m_dir->filePath("never-release"), {});
        for (const QString& key : keys) {
            option(key, "code", 0);
            option(key, "output", "");
        }
        refresh();
        QTRY_VERIFY(!populated());
    }

    void savedWifiUsesExistingProfileAndFriendlyErrors()
    {
        makeModel();
        auto* network = m_model.get();
        network->refreshWifi();
        QTRY_VERIFY(!network->wifiNetworks().isEmpty());
        auto knownNetwork = [&] {
            for (const QVariant& item : network->wifiNetworks()) {
                const QVariantMap wifi = item.toMap();
                if (wifi.value("ssid").toString() == QStringLiteral("Home:Lab")) {
                    return wifi;
                }
            }
            return QVariantMap();
        };
        QVERIFY(knownNetwork().value("known").toBool());

        // A saved profile without a usable secret must offer a retry instead
        // of displaying raw, potentially localized nmcli implementation text.
        option("connect", "code", 4);
        option("connect", "error",
            "Error: password for '802-11-wireless-security.psk' not provided in passwd-file");
        network->connectWifi("Home:Lab", {});
        QTRY_VERIFY(!network->connectResult().isEmpty());
        QCOMPARE(network->passwordRequiredSsid(), QString("Home:Lab"));
        QVERIFY(!network->connectResult().contains("psk"));
        QVERIFY(!network->connectResult().contains("passwd-file"));
        QCOMPARE(count("connect"), 1);
        auto lastConnectArgs = [&] {
            QJsonArray args;
            for (const QJsonObject& record : requests()) {
                if (record.value("operation").toString() == QStringLiteral("connect")) {
                    args = record.value("arguments").toArray();
                }
            }
            return args;
        };
        const QJsonArray savedArgs = lastConnectArgs();
        QCOMPARE(savedArgs, QJsonArray::fromStringList(
            { "connection", "up", "id", "Home:Lab" }));

        // With a newly entered password, use the explicit credential flow.
        option("connect", "code", 0);
        option("connect", "error", "");
        network->connectWifi("Home:Lab", "example-password");
        QTRY_COMPARE(network->connectResult(), QString("ok"));
        QCOMPARE(network->passwordRequiredSsid(), QString());
        QCOMPARE(count("connect"), 2);
        const QJsonArray retryArgs = lastConnectArgs();
        QCOMPARE(retryArgs, QJsonArray::fromStringList(
            { "device", "wifi", "connect", "Home:Lab", "password", "example-password" }));
    }

    void slowWifiScanKeepsCachedNetworksVisible()
    {
        makeModel();
        auto* network = m_model.get();
        option("scan", "gate", "finish-scan");
        network->scan();
        QVERIFY(network->scanning());
        // The slow hardware scan must not hold back cached NetworkManager APs.
        QTRY_VERIFY(!network->wifiNetworks().isEmpty());
        QVERIFY(network->scanning());
        QTRY_COMPARE(count("scan"), 1);
        saveFile(m_dir->filePath("finish-scan"), {});
        QTRY_VERIFY(!network->scanning());
        QTRY_VERIFY(!network->wifiNetworks().isEmpty());
    }

    void missingProgramCompletesAndRecovers()
    {
        makeModel();
        refresh();
        QTRY_VERIFY(populated());
        const QVariantMap before = cache();
        QVERIFY(QFile::remove(m_dir->filePath("nmcli")));
        auto* network = m_model.get();
        network->scan();
        QVERIFY(network->scanning());
        network->connectWifi("Home:Lab", {});
        QTRY_VERIFY(!network->scanning());
        QTRY_VERIFY(!network->connectResult().isEmpty());
        QVERIFY(network->connectResult() != "ok");
        QCOMPARE(cache(), before);
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), m_dir->filePath("nmcli")));
        network->scan();
        QTRY_VERIFY(!network->scanning());
        network->connectWifi("Home:Lab", {});
        QTRY_COMPARE(network->connectResult(), QString("ok"));
    }

    void destroyingOwnerDoesNotWait()
    {
        gateReads();
        makeModel();
        refresh();
        QTRY_VERIFY(count("devices") > 0);
        QElapsedTimer elapsed;
        elapsed.start();
        m_model.reset();
        QVERIFY2(elapsed.elapsed() < 250, "destroying a model must not wait for slow service processes");
        QTRY_VERIFY(clientsStopped());
    }

    void processTimeoutAndFailedStartupCompleteOnce()
    {
        QObject owner;
        int completions = 0;
        option("devices", "gate", "never-release");
        vela::runProcess(&owner, "nmcli", { "device" }, 80, [&](vela::ProcessResult result) {
            QVERIFY(!result.ok);
            ++completions;
        });
        vela::runProcess(&owner, "missing-executable", {}, 80, [&](vela::ProcessResult result) {
            QVERIFY(!result.ok);
            QVERIFY(!result.error.isEmpty());
            ++completions;
        });
        QTRY_COMPARE(completions, 2);
        QTest::qWait(100);
        QCOMPARE(completions, 2);
        QTRY_VERIFY(clientsStopped());
    }

};

int main(int argc, char* argv[])
{
    const QString program = QFileInfo(QString::fromLocal8Bit(argv[0])).fileName();
    QCoreApplication app(argc, argv);
    if (program == "nmcli") {
        return fakeService(app.arguments().mid(1));
    }
    ServiceModels tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "service_models.moc"
