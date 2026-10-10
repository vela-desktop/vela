// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Real QProcess/D-Bus clients talking to isolated, controllably slow services.
// No running PipeWire, NetworkManager or BlueZ instance is touched.
#include "appmodel.h"
#include "asyncprocess.h"
#include "audio.h"
#include "mixer.h"
#include "network.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDBusVirtualObject>
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

QJsonObject volume(int percent)
{
    return { { "front-left", QJsonObject { { "value", qRound(percent * 65536.0 / 100) } } } };
}

QString operation(const QString& program, const QStringList& args)
{
    if (program == "pw-metadata") {
        return "targets";
    }
    if (program == "pactl") {
        if (args.contains("list")) {
            return args.last() == "sink-inputs" ? "streams" : args.last();
        }
        return args.value(0);
    }
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

int fakeService(const QString& program, const QStringList& arguments)
{
    const QString key = operation(program, arguments);
    // Capture the response before waiting: it can become obsolete in flight.
    const QJsonObject response = configuration().value(key).toObject();
    QByteArray output = response.value("output").toString().toUtf8();
    if (key == "sinks" || key == "streams") {
        const QByteArray saved = readFile(serviceRoot() + "/volume").trimmed();
        if (!saved.isEmpty()) {
            QJsonArray list = QJsonDocument::fromJson(output).array();
            for (qsizetype i = 0; i < list.size(); ++i) {
                QJsonObject device = list[i].toObject();
                device.insert("volume", volume(saved.toInt()));
                list[i] = device;
            }
            output = QJsonDocument(list).toJson(QJsonDocument::Compact);
        }
    }
    logRequest(key, arguments);
    if (key == "subscribe") {
        // Feed real QProcess subscription events to the model from an
        // isolated test file; neither PipeWire nor PulseAudio is contacted.
        QTimer events;
        events.setInterval(10);
        qsizetype sent = 0;
        QObject::connect(&events, &QTimer::timeout, &events, [&] {
            const QByteArray data = readFile(serviceRoot() + "/subscription-events");
            if (data.size() < sent) {
                sent = 0;
            }
            if (data.size() == sent) {
                return;
            }
            const QByteArray next = data.mid(sent);
            sent = data.size();
            logRequest("subscription-output");
            ::write(STDOUT_FILENO, next.constData(), next.size());
        });
        events.start();
        return QCoreApplication::exec();
    }
    const QString gate = response.value("gate").toString();
    while (!gate.isEmpty() && !QFileInfo::exists(serviceRoot() + '/' + gate)) {
        QThread::msleep(5); // deliberately slow service, in a separate process
    }
    QThread::msleep(response.value("delay").toInt());
    if (key.startsWith("set-") && key.endsWith("volume")) {
        saveFile(serviceRoot() + "/volume", arguments.last().chopped(1).toUtf8());
    }
    ::write(STDOUT_FILENO, output.constData(), output.size());
    const QByteArray error = response.value("error").toString().toUtf8();
    ::write(STDERR_FILENO, error.constData(), error.size());
    return response.value("code").toInt();
}

class FakeBlueZ : public QDBusVirtualObject {
public:
    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
    {
        if (message.interface() != "org.freedesktop.DBus.Properties" || message.member() != "Get") {
            return false;
        }
        const QJsonObject response = configuration().value("bluez").toObject();
        logRequest("bluez");
        message.setDelayedReply(true);
        QTimer::singleShot(response.value("delay").toInt(), this, [message, connection, response] {
            if (response.value("fail").toBool()) {
                connection.send(message.createErrorReply("org.bluez.Error.Failed", "unavailable"));
            } else {
                connection.send(message.createReply(QVariant::fromValue(QDBusVariant(
                    QVariant::fromValue(uchar(response.value("battery").toInt()))))));
            }
        });
        return true;
    }
};

} // namespace

class ServiceModels : public QObject {
    Q_OBJECT

    enum Model { MixerModel, AudioModel, NetworkModel };
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<AppModel> m_apps;
    std::unique_ptr<QObject> m_model;
    QJsonObject m_config;
    QByteArray m_path;
    QProcess m_bus;
    QProcess m_bluez;
    int m_kind = MixerModel;

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
            if (record.value("operation").toString() != "bluez"
                && ::kill(record.value("pid").toInt(), 0) == 0) {
                return false;
            }
        }
        return true;
    }
    void makeModel(int kind)
    {
        m_kind = kind;
        if (kind == MixerModel) {
            m_model = std::make_unique<Mixer>(m_apps.get());
        } else if (kind == AudioModel) {
            m_model = std::make_unique<Audio>();
        } else {
            m_model = std::make_unique<Network>();
        }
        QVERIFY(m_model->property("available").toBool());
    }
    void refresh()
    {
        if (m_kind == MixerModel) {
            static_cast<Mixer*>(m_model.get())->setActive(true);
        } else if (m_kind == AudioModel) {
            static_cast<Audio*>(m_model.get())->refresh();
        } else {
            static_cast<Network*>(m_model.get())->refresh();
        }
    }
    QVariantMap cache() const
    {
        const QStringList properties = m_kind == MixerModel ? QStringList { "outputs", "apps" }
            : m_kind == AudioModel ? QStringList { "outputs", "inputs" } : QStringList { "devices", "wifiNetworks" };
        QVariantMap result;
        for (const QString& property : properties) {
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
        for (const QString key : { "get-default-sink", "get-default-source", "sinks", "sources", "streams",
                 "targets", "devices", "details", "known", "wifi" }) {
            option(key, "gate", "release");
        }
    }
    static void models_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("mixer") << int(MixerModel);
        QTest::newRow("settings-audio") << int(AudioModel);
        QTest::newRow("network") << int(NetworkModel);
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
        for (const QString program : { "pactl", "pw-metadata", "nmcli" }) {
            QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), m_dir->filePath(program)));
        }
        qputenv("PATH", QFile::encodeName(m_dir->path()));
        qputenv("VELA_TEST_SERVICES", QFile::encodeName(m_dir->path()));
        m_config = {};
        const QJsonObject sink { { "index", 1 }, { "name", "test-sink" }, { "description", "Speakers" },
            { "volume", volume(40) }, { "mute", false }, { "properties", QJsonObject { { "object.id", "10" }, { "object.serial", "20" } } } };
        response("sinks", QJsonDocument(QJsonArray { sink }).toJson());
        QJsonObject source = sink;
        source.insert("name", "test-mic");
        QJsonObject monitor = source;
        monitor.insert("name", "test-sink.monitor");
        response("sources", QJsonDocument(QJsonArray { source, monitor }).toJson());
        const QJsonObject stream { { "index", 7 }, { "sink", 1 }, { "volume", volume(40) }, { "mute", false },
            { "properties", QJsonObject { { "object.id", "99" }, { "application.process.binary", "vela-async-player" },
                { "application.name", "Async Player" } } } };
        response("streams", QJsonDocument(QJsonArray { stream }).toJson());
        response("get-default-sink", "test-sink\n");
        response("get-default-source", "test-mic\n");
        response("targets", "id:99 key:'target.object' value:'test-sink'\n");
        response("devices", "eth0:ethernet:connected:Wired\nwlan0:wifi:connected:Home\\:Lab\nlo:loopback:connected:lo\n");
        response("details", "GENERAL.HWADDR:AA\\:BB\nIP4.ADDRESS[1]:192.0.2.2/24\nIP4.GATEWAY:192.0.2.1\nIP4.DNS[1]:192.0.2.53\nIP6.ADDRESS[1]:2001\\:db8\\:\\:1/64\n");
        response("known", "Home\\:Lab:802-11-wireless\n");
        response("wifi", "*:Home\\:Lab:70:WPA2\n:Other:90:--\n");
        response("scan", "*:Home\\:Lab:70:WPA2\n:Other:90:--\n");
    }
    void cleanup()
    {
        m_model.reset();
        m_apps.reset();
        if (m_bluez.state() != QProcess::NotRunning) {
            m_bluez.kill();
            QVERIFY(m_bluez.waitForFinished());
        }
        QTRY_VERIFY(clientsStopped());
        qputenv("PATH", m_path);
        qunsetenv("VELA_TEST_SERVICES");
        m_dir.reset();
    }

    void delayedRefreshKeepsCacheAndEventLoop_data() { models_data(); }
    void delayedRefreshKeepsCacheAndEventLoop()
    {
        QFETCH(int, kind);
        makeModel(kind);
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
        const QString query = kind == NetworkModel ? "devices" : "sinks";
        QCOMPARE(count(query), 1);
        saveFile(m_dir->filePath("release"), {});
        QTRY_COMPARE(count(query), 2);
        QTRY_VERIFY(populated());
        QCOMPARE(cache(), before);
        if (kind == AudioModel) {
            QCOMPARE(static_cast<Audio*>(m_model.get())->inputs().size(), 1); // omit monitors
        } else if (kind == NetworkModel) {
            const Network* network = static_cast<Network*>(m_model.get());
            QCOMPARE(network->devices().size(), 2);
            QCOMPARE(network->devices().first().toMap().value("mac").toString(), QString("AA:BB"));
            QCOMPARE(network->wifiNetworks().first().toMap().value("ssid").toString(), QString("Home:Lab"));
            QVERIFY(network->wifiNetworks().first().toMap().value("known").toBool());
        }
    }

    void streamEventsDoNotInvalidateAudioSnapshot()
    {
        makeModel(AudioModel);
        auto* audio = static_cast<Audio*>(m_model.get());
        audio->refresh();
        QTRY_VERIFY(populated());
        QTRY_COMPARE(count("subscribe"), 1);
        saveFile(m_dir->filePath("requests.jsonl"), {});

        QJsonArray sinks = QJsonDocument::fromJson(
            m_config.value("sinks").toObject().value("output").toString().toUtf8()).array();
        QJsonObject sink = sinks.first().toObject();
        sink.insert("description", "Fresh speakers");
        sinks[0] = sink;
        option("sinks", "output", QString::fromUtf8(QJsonDocument(sinks).toJson()));
        option("sinks", "gate", "release");

        audio->refresh();
        QTRY_COMPARE(count("sinks"), 1);
        saveFile(m_dir->filePath("subscription-events"),
            "Event 'change' on sink-input #7\\nEvent 'change' on source-output #3\\n");
        QTRY_COMPARE(count("subscription-output"), 1);
        QTest::qWait(50); // allow QProcess stdout to reach the model
        saveFile(m_dir->filePath("release"), {});
        QTRY_COMPARE(audio->outputs().first().toMap().value("description").toString(), QString("Fresh speakers"));
        QTest::qWait(120);
        QCOMPARE(count("sinks"), 1); // no invalidation or extra query

        // A genuine device event must still trigger a fresh snapshot.
        sink.insert("description", "New speakers");
        sinks[0] = sink;
        option("sinks", "output", QString::fromUtf8(QJsonDocument(sinks).toJson()));
        saveFile(m_dir->filePath("subscription-events"),
            "Event 'change' on sink-input #7\\nEvent 'change' on source-output #3\\n"
            "Event 'change' on sink #1\\n");
        QTRY_COMPARE(count("subscription-output"), 2);
        QTRY_COMPARE(audio->outputs().first().toMap().value("description").toString(), QString("New speakers"));
    }

    void failedRefreshKeepsCache_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("failure");
        for (int kind : { MixerModel, AudioModel, NetworkModel }) {
            for (const QString failure : { "exit", "invalid", "timeout" }) {
                const QByteArray name = QByteArray::number(kind) + '-' + failure.toLatin1();
                QTest::newRow(name.constData()) << kind << failure;
            }
        }
    }
    void failedRefreshKeepsCache()
    {
        QFETCH(int, kind);
        QFETCH(QString, failure);
        makeModel(kind);
        refresh();
        QTRY_VERIFY(populated());
        const QVariantMap before = cache();
        const QStringList keys = kind == NetworkModel ? QStringList { "devices", "wifi" } : QStringList { "sinks" };
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
        QTest::qWait(failure == "timeout" ? (kind == MixerModel ? 1300 : kind == AudioModel ? 2300 : 3300) : 150);
        QVERIFY(beats >= 5);
        QCOMPARE(cache(), before);
        // Failed/time-limited requests release their busy state and can retry.
        saveFile(m_dir->filePath("never-release"), {});
        for (const QString& key : keys) {
            option(key, "code", 0);
            option(key, "output", kind == NetworkModel ? "" : "[]");
        }
        refresh();
        QTRY_VERIFY(!populated());
    }

    void oldReadCannotUndoLatestVolume_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("mixer") << int(MixerModel);
        QTest::newRow("settings-audio") << int(AudioModel);
    }
    void oldReadCannotUndoLatestVolume()
    {
        QFETCH(int, kind);
        makeModel(kind);
        refresh();
        QTRY_VERIFY(populated());
        saveFile(m_dir->filePath("requests.jsonl"), {});
        gateReads();
        refresh();
        QTRY_COMPARE(count("sinks"), 1);
        const QString command = kind == MixerModel ? "set-sink-input-volume" : "set-sink-volume";
        option(command, "gate", "commands-release");
        auto setVolume = [&](double value) {
            if (kind == MixerModel) {
                static_cast<Mixer*>(m_model.get())->setAppVolume("vela-async-player", value);
            } else {
                static_cast<Audio*>(m_model.get())->setVolume("output", "test-sink", value);
            }
        };
        auto currentVolume = [&] {
            return qRound(100 * m_model->property(kind == MixerModel ? "apps" : "outputs").toList().first().toMap().value("volume").toDouble());
        };
        setVolume(0.6);
        setVolume(0.7);
        setVolume(0.8);
        QCOMPARE(currentVolume(), 80);
        QTRY_COMPARE(count(command), 1);
        saveFile(m_dir->filePath("release"), {});
        QTest::qWait(80); // the captured 40% reply arrives while the command is pending
        QCOMPARE(currentVolume(), 80);
        saveFile(m_dir->filePath("commands-release"), {});
        QTRY_COMPARE(count(command), 2);
        QTRY_COMPARE(readFile(m_dir->filePath("volume")), QByteArray("80"));
        QTRY_COMPARE(count("sinks"), 2);
        QCOMPARE(currentVolume(), 80);
        QStringList volumes;
        for (const QJsonObject& request : requests()) {
            if (request.value("operation").toString() == command) {
                volumes.append(request.value("arguments").toArray().last().toString());
            }
        }
        QCOMPARE(volumes, QStringList({ "60%", "80%" }));
    }

    void taskWheelWaitsForStreams()
    {
        // 50% is an exact PulseAudio value (32768): this test concerns the
        // deferred first notch, without rounding just below a grid boundary.
        QJsonArray streams = QJsonDocument::fromJson(m_config.value("streams").toObject().value("output").toString().toUtf8()).array();
        QJsonObject stream = streams.first().toObject();
        stream.insert("volume", volume(50));
        response("streams", QJsonDocument(QJsonArray { stream }).toJson());
        const QByteArray oldData = qgetenv("XDG_DATA_HOME");
        const QString data = m_dir->filePath("data");
        QDir().mkpath(data + "/applications");
        saveFile(data + "/applications/org.vela.async-player.desktop",
            "[Desktop Entry]\nType=Application\nName=Async Player\nExec=vela-async-player\n");
        qputenv("XDG_DATA_HOME", QFile::encodeName(data));
        m_apps = std::make_unique<AppModel>();
        m_apps->reload();
        qputenv("XDG_DATA_HOME", oldData);
        makeModel(MixerModel);
        auto* mixer = static_cast<Mixer*>(m_model.get());
        QSignalSpy osd(mixer, &Mixer::appOsdRequested);
        QVERIFY(mixer->stepTaskVolume("org.vela.async-player.desktop", 1));
        QVERIFY(mixer->apps().isEmpty());
        QTRY_COMPARE(osd.size(), 1);
        QCOMPARE(qRound(100 * mixer->apps().first().toMap().value("volume").toDouble()), 52);
        QTRY_COMPARE(readFile(m_dir->filePath("volume")), QByteArray("52"));
    }

    void savedWifiUsesExistingProfileAndFriendlyErrors()
    {
        makeModel(NetworkModel);
        auto* network = static_cast<Network*>(m_model.get());
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
        makeModel(NetworkModel);
        auto* network = static_cast<Network*>(m_model.get());
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
        makeModel(NetworkModel);
        refresh();
        QTRY_VERIFY(populated());
        const QVariantMap before = cache();
        QVERIFY(QFile::remove(m_dir->filePath("nmcli")));
        auto* network = static_cast<Network*>(m_model.get());
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

    void destroyingOwnerDoesNotWait_data() { models_data(); }
    void destroyingOwnerDoesNotWait()
    {
        QFETCH(int, kind);
        gateReads();
        makeModel(kind);
        refresh();
        QTRY_VERIFY(count(kind == NetworkModel ? "devices" : "sinks") > 0);
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
        option("get-default-sink", "gate", "never-release");
        vela::runProcess(&owner, "pactl", { "get-default-sink" }, 80, [&](vela::ProcessResult result) {
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

    void failedDefaultMicrophoneCannotRetargetMute()
    {
        // Two microphones make it possible to distinguish the confirmed
        // default from an optimistically selected (but rejected) replacement.
        QJsonArray sources = QJsonDocument::fromJson(
            m_config.value("sources").toObject().value("output").toString().toUtf8()).array();
        QJsonObject backup = sources.first().toObject();
        backup.insert("name", "backup-mic");
        backup.insert("description", "Backup microphone");
        sources.append(backup);
        response("sources", QJsonDocument(sources).toJson());
        makeModel(AudioModel);
        auto* audio = static_cast<Audio*>(m_model.get());
        audio->refresh();
        QTRY_COMPARE(audio->inputs().size(), 2);

        auto device = [&](const QString& name) {
            for (const QVariant& value : audio->inputs()) {
                const QVariantMap item = value.toMap();
                if (item.value("name").toString() == name) {
                    return item;
                }
            }
            return QVariantMap();
        };
        QVERIFY(device("test-mic").value("isDefault").toBool());
        QVERIFY(!device("backup-mic").value("isDefault").toBool());

        option("set-default-source", "code", 3);
        option("get-default-source", "code", 3);
        audio->setDefault("input", "backup-mic");
        QVERIFY(device("test-mic").value("isDefault").toBool());
        QVERIFY(!device("backup-mic").value("isDefault").toBool());
        QVERIFY(device("test-mic").value("defaultPending").toBool());

        QTRY_VERIFY(device("test-mic").value("defaultError").toBool());
        QVERIFY(!device("test-mic").value("defaultPending").toBool());
        QVERIFY(device("test-mic").value("isDefault").toBool());
        QVERIFY(!device("backup-mic").value("isDefault").toBool());

        // Even a direct QML/API mute request must not act on an unconfirmed
        // default selection (the real microphone could still be test-mic).
        const int before = count("set-source-mute");
        audio->setMuted("input", "test-mic", true);
        QCOMPARE(count("set-source-mute"), before);
        audio->setVolume("input", "test-mic", 0.0);
        QCOMPARE(count("set-source-volume"), 0);

        option("get-default-source", "code", 0);
        audio->refresh();
        QTRY_VERIFY(!device("test-mic").value("defaultError").toBool());
        QVERIFY(device("test-mic").value("isDefault").toBool());
        audio->setMuted("input", "test-mic", true);
        QTRY_COMPARE(count("set-source-mute"), before + 1);
    }

    void successfulDefaultChangeNeedsConfirmedRead()
    {
        QJsonArray sources = QJsonDocument::fromJson(
            m_config.value("sources").toObject().value("output").toString().toUtf8()).array();
        QJsonObject backup = sources.first().toObject();
        backup.insert("name", "backup-mic");
        sources.append(backup);
        response("sources", QJsonDocument(sources).toJson());
        makeModel(AudioModel);
        auto* audio = static_cast<Audio*>(m_model.get());
        audio->refresh();
        QTRY_COMPARE(audio->inputs().size(), 2);
        const auto devices = [&] { return audio->inputs(); };
        const int reads = count("get-default-source");

        // pactl accepts the change, but follow-up reads are unavailable.
        option("get-default-source", "code", 3);
        audio->setDefault("input", "backup-mic");
        QTRY_VERIFY(count("get-default-source") > reads);
        QTRY_VERIFY(devices().first().toMap().value("defaultPending").toBool());
        QVERIFY(devices().first().toMap().value("isDefault").toBool());
        audio->setMuted("input", "test-mic", true);
        QCOMPARE(count("set-source-mute"), 0);

        // Only an authoritative snapshot may move the default to backup-mic
        // and re-enable input mute operations.
        response("get-default-source", "backup-mic\n");
        audio->refresh();
        QTRY_VERIFY(devices().last().toMap().value("isDefault").toBool());
        QVERIFY(!devices().first().toMap().value("defaultPending").toBool());
        QVERIFY(!devices().first().toMap().value("defaultError").toBool());
        QVERIFY(!devices().first().toMap().value("isDefault").toBool());
        audio->setMuted("input", "backup-mic", true);
        QTRY_COMPARE(count("set-source-mute"), 1);
    }

    void failedMicrophoneMuteMustNotClaimSuccess()
    {
        makeModel(AudioModel);
        refresh();
        auto* audio = static_cast<Audio*>(m_model.get());
        QTRY_VERIFY(populated());
        auto mic = [&] { return audio->inputs().first().toMap(); };
        QVERIFY(!mic().value("muted").toBool());

        // A failing command followed by a failing reconciliation must never
        // make the visible input appear safely muted.
        option("set-source-mute", "code", 3);
        option("sources", "code", 3);
        audio->setMuted("input", "test-mic", true);
        QCOMPARE(mic().value("muted").toBool(), false);
        QTRY_VERIFY(mic().value("muteError").toBool());
        QVERIFY(!mic().value("mutePending").toBool());
        QVERIFY(!mic().value("muted").toBool());

        // Service recovery clears the error only after a valid source snapshot.
        option("sources", "code", 0);
        audio->refresh();
        QTRY_VERIFY(!mic().value("muteError").toBool());
        QVERIFY(!mic().value("muted").toBool());
    }

    void failedSubscriptionCanRestart()
    {
        makeModel(AudioModel);
        auto* audio = static_cast<Audio*>(m_model.get());
        QVERIFY(QFile::remove(m_dir->filePath("pactl")));
        audio->refresh();
        QTest::qWait(100);
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), m_dir->filePath("pactl")));
        audio->refresh();
        QTRY_COMPARE(count("subscribe"), 1);
        QTRY_VERIFY(populated());
    }

    void missingPipeWireMetadataStillUpdatesMixer()
    {
        makeModel(MixerModel);
        refresh();
        QTRY_VERIFY(populated());
        QVERIFY(QFile::remove(m_dir->filePath("pw-metadata")));
        QJsonObject sink = QJsonDocument::fromJson(m_config.value("sinks").toObject().value("output").toString().toUtf8()).array().first().toObject();
        sink.insert("description", "Updated speakers");
        response("sinks", QJsonDocument(QJsonArray { sink }).toJson());
        refresh();
        auto* mixer = static_cast<Mixer*>(m_model.get());
        QTRY_COMPARE(mixer->outputs().first().toMap().value("description").toString(), QString("Updated speakers"));
        QCOMPARE(mixer->apps().first().toMap().value("output").toString(), QString("test-sink"));
    }

    void delayedBatteryDoesNotBlockOrOverwriteNewerReply()
    {
        QJsonArray sinks = QJsonDocument::fromJson(m_config.value("sinks").toObject().value("output").toString().toUtf8()).array();
        QJsonObject sink = sinks.first().toObject();
        QJsonObject properties = sink.value("properties").toObject();
        properties.insert("api.bluez5.address", "AA:BB:CC:DD:EE:FF");
        sink.insert("properties", properties);
        response("sinks", QJsonDocument(QJsonArray { sink }).toJson());
        m_config.insert("bluez", QJsonObject { { "delay", 200 }, { "battery", 73 } });
        save();
        m_bluez.start(QCoreApplication::applicationFilePath(), { "--fake-bluez" });
        QVERIFY(m_bluez.waitForStarted());
        QVERIFY(m_bluez.waitForReadyRead());
        QVERIFY(m_bluez.readAllStandardOutput().contains("ready"));
        makeModel(MixerModel);
        auto* mixer = static_cast<Mixer*>(m_model.get());
        auto battery = [&] { return mixer->outputs().first().toMap().value("battery").toInt(); };
        int beats = 0;
        qint64 largestGap = 0;
        QElapsedTimer gap;
        gap.start();
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] {
            largestGap = qMax(largestGap, gap.restart());
            ++beats;
        });
        heartbeat.start(5);
        refresh();
        QTRY_VERIFY(populated());
        QCOMPARE(battery(), -1); // outputs arrive before BlueZ
        QTRY_COMPARE(battery(), 73);
        QVERIFY(beats >= 10);
        QVERIFY2(largestGap < 150, "BlueZ must not stop the GUI event loop");

        m_config.insert("bluez", QJsonObject { { "delay", 200 }, { "battery", 20 } });
        save();
        refresh();
        QTRY_COMPARE(count("bluez"), 2);
        QCOMPARE(battery(), 73); // retain the last charge while waiting
        m_config.insert("bluez", QJsonObject { { "battery", 90 } });
        save();
        refresh();
        QTRY_COMPARE(battery(), 90);
        QTest::qWait(250);
        QCOMPARE(battery(), 90); // the earlier delayed 20% reply is obsolete
    }
};

int main(int argc, char* argv[])
{
    const QString program = QFileInfo(QString::fromLocal8Bit(argv[0])).fileName();
    QCoreApplication app(argc, argv);
    if (program == "pactl" || program == "nmcli" || program == "pw-metadata") {
        return fakeService(program, app.arguments().mid(1));
    }
    if (app.arguments().contains("--fake-bluez")) {
        FakeBlueZ bluez;
        QDBusConnection bus = QDBusConnection::systemBus();
        if (!bus.registerService("org.bluez") || !bus.registerVirtualObject("/", &bluez, QDBusConnection::SubPath)) {
            return 1;
        }
        ::write(STDOUT_FILENO, "ready\n", 6);
        return app.exec();
    }
    ServiceModels tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "service_models.moc"
