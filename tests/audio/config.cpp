// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Vela's files and the sample rate (docs/audio.md §8) against a real
// PipeWire and WirePlumber: what devices can run at and run at now, the
// graph following what plays once "Automatic", its default, WirePlumber's
// file written whole and read back, and the restarts waiting for
// WirePlumber to save its state.

#include "audio.h"
#include "config.h"
#include "core.h"
#include "harness.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>

using namespace harness;
using vela::audio::Audio;
using vela::audio::Config;
using vela::audio::DeviceRule;

namespace {

QVariantMap find(const QVariantList& list, const QString& field, const QString& value)
{
    for (const QVariant& item : list) {
        if (item.toMap().value(field).toString() == value) {
            return item.toMap();
        }
    }
    return {};
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QList<quint32> rates(const QVariantList& list)
{
    QList<quint32> out;
    for (const QVariant& v : list) {
        out.append(v.toUInt());
    }
    return out;
}

const QList<quint32> automatic { 44100, 48000, 88200, 96000, 176400, 192000 };

} // namespace

class ConfigTest : public QObject {
    Q_OBJECT

    std::unique_ptr<Audio> audio;
    // The restarts asked for, and when.
    QStringList restarts;
    QElapsedTimer clock;
    QList<qint64> restartTimes;

    QVariantMap output(const QString& name) const { return find(audio->outputs(), QStringLiteral("name"), name); }
    QList<quint32> serverRates() const { return audio->core()->snapshot().allowedRates; }
    // The graph's rates straight in PipeWire's metadata, as another program
    // (or the user's own configuration) would set them.
    static void setServerRates(const QString& rates)
    {
        run(QStringLiteral("pw-metadata"), { QStringLiteral("-n"), QStringLiteral("settings"), QStringLiteral("0"),
                                               QStringLiteral("clock.allowed-rates"), rates });
    }
    void resetRates()
    {
        QFile::remove(Config::pipewireFile());
        run(QStringLiteral("pw-metadata"), { QStringLiteral("-n"), QStringLiteral("settings"), QStringLiteral("0"),
                                               QStringLiteral("clock.rate"), QStringLiteral("48000") });
        setServerRates(QStringLiteral("[ 48000 ]"));
        QVERIFY(QTest::qWaitFor([this] { return serverRates() == QList<quint32> { 48000 }; }, 5000));
    }
    std::unique_ptr<Audio> newAudio()
    {
        auto a = std::make_unique<Audio>();
        a->config()->setRestarter([this](const QString& unit) {
            restarts.append(unit);
            restartTimes.append(clock.elapsed());
        });
        return a;
    }

private slots:
    void initTestCase()
    {
        // Vela's files go where the fixture's WirePlumber and PipeWire look.
        QVERIFY(Config::wireplumberFile().startsWith(qEnvironmentVariable("XDG_CONFIG_HOME")));
        clock.start();
        audio = newAudio();
        QVERIFY(QTest::qWaitFor([this] { return audio->available() && !output(speakers).isEmpty(); }, 10000));
        QVERIFY(QTest::qWaitFor([this] { return audio->core()->snapshot().settingsKnown; }, 5000));
    }

    void init()
    {
        restarts.clear();
        restartTimes.clear();
    }

    void parsesSpaJson()
    {
        const QVariantMap file = Config::parse("# a comment\n"
                                               "context.properties = {\n"
                                               "    default.clock.allowed-rates = [ 44100, 48000 ]\n"
                                               "}\n"
                                               "monitor.alsa.rules = [\n"
                                               "  { matches = [ { node.name = \"alsa_output.\\\"odd\\\" \\\\ name\" } ]\n"
                                               "    actions = { update-props = { node.description = Bare audio.format = \"S16LE\" } } }\n"
                                               "]\n")
                                     .toMap();
        QCOMPARE(file.value(QStringLiteral("context.properties")).toMap().value(QStringLiteral("default.clock.allowed-rates")),
            QVariant(QVariantList { 44100, 48000 }));
        const QVariantMap rule = file.value(QStringLiteral("monitor.alsa.rules")).toList().value(0).toMap();
        QCOMPARE(rule.value(QStringLiteral("matches")).toList().value(0).toMap().value(QStringLiteral("node.name")).toString(),
            QStringLiteral("alsa_output.\"odd\" \\ name"));
        const QVariantMap props = rule.value(QStringLiteral("actions")).toMap().value(QStringLiteral("update-props")).toMap();
        QCOMPARE(props.value(QStringLiteral("node.description")).toString(), QStringLiteral("Bare"));
        QCOMPARE(props.value(QStringLiteral("audio.format")).toString(), QStringLiteral("S16LE"));
        QVERIFY(Config::parse("").toMap().isEmpty());
        Config::parse("not = [ valid"); // broken: no crash
    }

    // What a device can run at: its rate range as standard rates, one
    // format per depth; Vela's record of what it offered once restricted.
    void capabilities()
    {
        const QVariantMap speakersMap = output(speakers);
        QCOMPARE(rates(speakersMap.value(QStringLiteral("rates")).toList()), automatic); // a null sink: any rate
        QCOMPARE(speakersMap.value(QStringLiteral("formats")).toList().size(), 1); // F32P and F32LE: one depth
        QCOMPARE(speakersMap.value(QStringLiteral("formats")).toList().value(0).toMap().value(QStringLiteral("bits")).toInt(), 32);
        QCOMPARE(speakersMap.value(QStringLiteral("canFormat")).toBool(), false); // not ALSA
        QCOMPARE(speakersMap.value(QStringLiteral("fixedRate")).toInt(), 0);

        QVERIFY(control(QStringLiteral("add-sink vela-test-dac FL FR device.api=alsa vela.audio.rates=[44100,96000] "
                                       "vela.audio.formats=[S16LE,S24_32LE] audio.allowed-rates=[96000] audio.format=S24_32LE"))
                    .startsWith(QLatin1String("ok")));
        QVERIFY(QTest::qWaitFor([this] { return !output(QStringLiteral("vela-test-dac")).isEmpty(); }, 5000));
        const QVariantMap dac = output(QStringLiteral("vela-test-dac"));
        QCOMPARE(dac.value(QStringLiteral("canFormat")).toBool(), true);
        QCOMPARE(rates(dac.value(QStringLiteral("rates")).toList()), (QList<quint32> { 44100, 96000 }));
        const QVariantList formats = dac.value(QStringLiteral("formats")).toList();
        QCOMPARE(formats.size(), 2);
        QCOMPARE(formats[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("S24_32LE")); // deepest first
        QCOMPARE(formats[1].toMap().value(QStringLiteral("bits")).toInt(), 16);
        QCOMPARE(dac.value(QStringLiteral("fixedRate")).toInt(), 96000);
        QCOMPARE(dac.value(QStringLiteral("fixedFormat")).toString(), QStringLiteral("S24_32LE"));
        QCOMPARE(control(QStringLiteral("remove vela-test-dac")), QStringLiteral("ok"));
    }

    // PipeWire's default converts everything to 48 kHz; "Automatic" lets
    // the graph run at what plays, a fixed rate converts everything to that
    // one, at once and for the next sessions.
    void graphFollowsWhatPlays()
    {
        resetRates();
        QCOMPARE(audio->sampleRateMode(), QStringLiteral("fixed"));
        {
            auto music = player(QStringLiteral("music"), { QStringLiteral("--rate"), QStringLiteral("44100") });
            QVERIFY(music->waitStreaming() != 0);
            QVERIFY(QTest::qWaitFor([this] { return output(speakers).value(QStringLiteral("rate")).toInt() == 48000
                                                 && !output(speakers).value(QStringLiteral("converted")).toList().isEmpty(); },
                5000));
            const QVariantMap converted = output(speakers).value(QStringLiteral("converted")).toList().first().toMap();
            QCOMPARE(converted.value(QStringLiteral("name")).toString(), QStringLiteral("music"));
            QCOMPARE(converted.value(QStringLiteral("rate")).toInt(), 44100);
        }

        audio->setSampleRate(0);
        QCOMPARE(audio->sampleRateMode(), QStringLiteral("automatic")); // shown at once
        QVERIFY(QTest::qWaitFor([this] { return serverRates() == automatic; }, 5000));
        QCOMPARE(Config().pipewireRates(), automatic);
        QVERIFY(restarts.isEmpty()); // no restart needed
        // The graph switches once idle.
        QVERIFY(QTest::qWaitFor([this] { return output(speakers).value(QStringLiteral("rate")).toInt() == 0; }, 10000));
        {
            auto music = player(QStringLiteral("music"), { QStringLiteral("--rate"), QStringLiteral("44100") });
            QVERIFY(music->waitStreaming() != 0);
            QVERIFY(QTest::qWaitFor([this] { return output(speakers).value(QStringLiteral("rate")).toInt() == 44100; }, 5000));
            QVERIFY(output(speakers).value(QStringLiteral("converted")).toList().isEmpty());
        }

        // Fixed at a higher rate: everything converted to it.
        audio->setSampleRate(96000);
        QCOMPARE(audio->sampleRate(), 96000); // shown at once
        QVERIFY(QTest::qWaitFor([this] { return serverRates() == QList<quint32> { 96000 }; }, 5000));
        QCOMPARE(Config().pipewireRates(), (QList<quint32> { 96000 }));
        {
            QFile file(Config::pipewireFile());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY(file.readAll().contains("default.clock.rate = 96000"));
        }
        QCOMPARE(audio->sampleRateMode(), QStringLiteral("fixed"));
        QVERIFY(restarts.isEmpty());
        QVERIFY(QTest::qWaitFor([this] { return output(speakers).value(QStringLiteral("rate")).toInt() == 0; }, 10000));
        {
            auto music = player(QStringLiteral("music"), { QStringLiteral("--rate"), QStringLiteral("44100") });
            QVERIFY(music->waitStreaming() != 0);
            QVERIFY(QTest::qWaitFor([this] { return output(speakers).value(QStringLiteral("rate")).toInt() == 96000; }, 5000));
        }

        audio->setSampleRate(48000);
        QVERIFY(QTest::qWaitFor([this] { return serverRates() == QList<quint32> { 48000 }; }, 5000));
        QCOMPARE(Config().pipewireRates(), (QList<quint32> { 48000 }));
        QCOMPARE(audio->sampleRate(), 48000);
        // Not a rate Vela offers: refused.
        audio->setSampleRate(32000);
        QCOMPARE(audio->sampleRate(), 48000);
        QCOMPARE(Config().pipewireRates(), (QList<quint32> { 48000 }));
        resetRates();
    }

    // The shell makes "Automatic" the default, only where nobody chose.
    void automaticByDefault()
    {
        resetRates();
        {
            auto shell = newAudio();
            shell->applyDefaultSampleRate();
            QVERIFY(QTest::qWaitFor([this] { return serverRates() == automatic; }, 5000));
            QCOMPARE(Config().pipewireRates(), automatic);
        }

        // The user chose (Vela's file is there): left alone.
        setServerRates(QStringLiteral("[ 48000 ]"));
        QVERIFY(QTest::qWaitFor([this] { return serverRates() == QList<quint32> { 48000 }; }, 5000));
        {
            auto shell = newAudio();
            shell->applyDefaultSampleRate();
            QVERIFY(QTest::qWaitFor([&] { return shell->sampleRateMode() == QLatin1String("fixed"); }, 5000));
            QTest::qWait(300);
            QCOMPARE(serverRates(), (QList<quint32> { 48000 }));
        }

        // Someone configured them another way: left alone, shown as they are.
        QFile::remove(Config::pipewireFile());
        setServerRates(QStringLiteral("[ 44100 48000 ]"));
        QVERIFY(QTest::qWaitFor([this] { return serverRates() == QList<quint32> { 44100, 48000 }; }, 5000));
        {
            auto shell = newAudio();
            shell->applyDefaultSampleRate();
            QVERIFY(QTest::qWaitFor([&] { return shell->sampleRateMode() == QLatin1String("custom"); }, 5000));
            QTest::qWait(300);
            QCOMPARE(serverRates(), (QList<quint32> { 44100, 48000 }));
            QVERIFY(!Config().hasPipewireFile());
        }
        resetRates();
    }

    // WirePlumber's file: Vela's rules only, rewritten whole, read back.
    void wireplumberFile()
    {
        const QString dir = QFileInfo(Config::wireplumberFile()).absolutePath();
        const QByteArray fixtureRules = read(dir + QStringLiteral("/10-vela-test.conf"));
        QVERIFY(!fixtureRules.isEmpty());

        audio->setEndpointName(QStringLiteral("output"), speakers, QStringLiteral("Living \"room\" \\ speakers"));
        QCOMPARE(output(speakers).value(QStringLiteral("description")).toString(),
            QStringLiteral("Living \"room\" \\ speakers")); // shown at once
        DeviceRule rule = Config().rule(speakers);
        QCOMPARE(rule.description, QStringLiteral("Living \"room\" \\ speakers"));
        QCOMPARE(rule.ownDescription, QStringLiteral("Test Speakers"));
        QVERIFY(!rule.bluetooth);
        QVERIFY(!rule.restricts());
        QVERIFY(!read(Config::wireplumberFile()).contains("vela.audio.rates")); // not restricted

        QVERIFY(control(QStringLiteral("add-sink vela-test-dac FL FR device.api=alsa")).startsWith(QLatin1String("ok")));
        QVERIFY(QTest::qWaitFor([this] { return !output(QStringLiteral("vela-test-dac")).isEmpty(); }, 5000));
        audio->setEndpointRate(QStringLiteral("output"), QStringLiteral("vela-test-dac"), 96000);
        QCOMPARE(output(QStringLiteral("vela-test-dac")).value(QStringLiteral("fixedRate")).toInt(), 96000);
        audio->setEndpointFormat(QStringLiteral("output"), QStringLiteral("vela-test-dac"), QStringLiteral("F32LE"));
        rule = Config().rule(QStringLiteral("vela-test-dac"));
        QCOMPARE(rule.allowedRates, (QList<quint32> { 96000 }));
        QCOMPARE(rule.format, QStringLiteral("F32LE"));
        QCOMPARE(rule.offeredRates, automatic); // to offer the others later
        QVERIFY(rule.offeredFormats.contains(QStringLiteral("F32LE")));
        QCOMPARE(Config().rules().size(), 2);
        // Not offered: refused, the file unchanged.
        const QByteArray before = read(Config::wireplumberFile());
        audio->setEndpointRate(QStringLiteral("output"), QStringLiteral("vela-test-dac"), 12345);
        audio->setEndpointRate(QStringLiteral("output"), speakers, 44100); // not ALSA
        QCOMPARE(read(Config::wireplumberFile()), before);

        // A Bluetooth headset's rule goes with WirePlumber's BlueZ monitor.
        QVERIFY(control(QStringLiteral("add-sink vela-test-headset FL FR api.bluez5.address=AA:BB:CC:DD:EE:FF")).startsWith(QLatin1String("ok")));
        QVERIFY(QTest::qWaitFor([this] { return !output(QStringLiteral("vela-test-headset")).isEmpty(); }, 5000));
        audio->setEndpointName(QStringLiteral("output"), QStringLiteral("vela-test-headset"), QStringLiteral("Cuffie"));
        QVERIFY(Config().rule(QStringLiteral("vela-test-headset")).bluetooth);
        QVERIFY(read(Config::wireplumberFile()).contains("monitor.bluez.rules"));

        // Back to the defaults: no rule left.
        audio->setEndpointName(QStringLiteral("output"), speakers, QString());
        audio->setEndpointName(QStringLiteral("output"), QStringLiteral("vela-test-headset"), QString());
        audio->setEndpointRate(QStringLiteral("output"), QStringLiteral("vela-test-dac"), 0);
        audio->setEndpointFormat(QStringLiteral("output"), QStringLiteral("vela-test-dac"), QString());
        QVERIFY(Config().rules().isEmpty());
        QVERIFY(!read(Config::wireplumberFile()).contains("monitor."));
        QCOMPARE(read(dir + QStringLiteral("/10-vela-test.conf")), fixtureRules);
        QStringList others = QDir(dir).entryList(QDir::Files);
        others.sort();
        QCOMPARE(others, (QStringList { QStringLiteral("10-vela-test.conf"), QStringLiteral("90-vela.conf") }));

        QCOMPARE(control(QStringLiteral("remove vela-test-dac")), QStringLiteral("ok"));
        QCOMPARE(control(QStringLiteral("remove vela-test-headset")), QStringLiteral("ok"));
    }

    // A new name restarts nothing: Vela shows the rule's at once, in every
    // process; the node keeps its old one until WirePlumber makes it again.
    void renameWithoutRestart()
    {
        QVERIFY(QTest::qWaitFor([this] { return !audio->restartPending(); }, 5000)); // the last test's
        restarts.clear();
        auto shell = newAudio();
        QVERIFY(QTest::qWaitFor([&] { return shell->available() && !shell->outputs().isEmpty(); }, 5000));
        const auto shellName = [&] {
            for (const QVariant& o : shell->outputs()) {
                if (o.toMap().value(QStringLiteral("name")).toString() == speakers) {
                    return o.toMap().value(QStringLiteral("description")).toString();
                }
            }
            return QString();
        };

        audio->setEndpointName(QStringLiteral("output"), speakers, QStringLiteral("Desk"));
        QCOMPARE(output(speakers).value(QStringLiteral("description")).toString(), QStringLiteral("Desk"));
        QVERIFY(QTest::qWaitFor([&] { return shellName() == QLatin1String("Desk"); }, 5000)); // the file watched
        QCOMPARE(audio->core()->snapshot().endpoint(speakers)->description, QStringLiteral("Test Speakers"));
        QVERIFY(!audio->restartPending());
        QTest::qWait(2000);
        QVERIFY(restarts.isEmpty());

        // Back to its own name: no rule left, since the node still has it.
        audio->setEndpointName(QStringLiteral("output"), speakers, QString());
        QCOMPARE(output(speakers).value(QStringLiteral("description")).toString(), QStringLiteral("Test Speakers"));
        QVERIFY(QTest::qWaitFor([&] { return shellName() == QLatin1String("Test Speakers"); }, 5000));
        QVERIFY(Config().rules().isEmpty());

        // A node made with a name from Vela's rule (here its node.name):
        // back to its own name, the rule says it until WirePlumber starts
        // again.
        const QString kitchen = QStringLiteral("vela-test-kitchen");
        QVERIFY(control(QStringLiteral("add-sink vela-test-kitchen FL FR device.api=alsa")).startsWith(QLatin1String("ok")));
        QVERIFY(QTest::qWaitFor([&] { return !output(kitchen).isEmpty(); }, 5000));
        DeviceRule old;
        old.node = kitchen;
        old.description = kitchen;
        old.ownDescription = QStringLiteral("Kitchen DAC");
        QVERIFY(Config().setRule(old));
        QVERIFY(QTest::qWaitFor([&] { return output(kitchen).value(QStringLiteral("description")) == kitchen; }, 5000));
        audio->setEndpointName(QStringLiteral("output"), kitchen, QString());
        QCOMPARE(output(kitchen).value(QStringLiteral("description")).toString(), QStringLiteral("Kitchen DAC"));
        QCOMPARE(Config().rule(kitchen).description, QStringLiteral("Kitchen DAC"));
        QCOMPARE(control(QStringLiteral("remove vela-test-kitchen")), QStringLiteral("ok"));
        DeviceRule none;
        none.node = kitchen;
        QVERIFY(Config().setRule(none));
        QVERIFY(Config().rules().isEmpty());
        QVERIFY(restarts.isEmpty());
    }

    // If applying live metadata fails, the next login must not apply a rate
    // that Settings already reported as rejected.
    void pipewireWriteRollsBack()
    {
        const QString path = Config::pipewireFile();
        const bool existed = QFileInfo::exists(path);
        const QByteArray original = read(path);
        Config cfg;
        QVERIFY(cfg.setPipewireRates({ 48000 }));
        const QByteArray before = read(path);
        bool invoked = false;
        QVERIFY(!cfg.setPipewireRates({ 96000 }, [&] {
            invoked = true;
            return false;
        }));
        QVERIFY(invoked);
        QCOMPARE(read(path), before);

        QVERIFY(QFile::remove(path));
        invoked = false;
        QVERIFY(!cfg.setPipewireRates({ 96000 }, [&] {
            invoked = true;
            return false;
        }));
        QVERIFY(invoked);
        QVERIFY(!QFileInfo::exists(path)); // no file before: none after
        if (existed) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(original), qint64(original.size()));
            file.close();
        }
    }

    // The fake ALSA node does not reload its WirePlumber rule, as can happen
    // with a failed restart. Optimism must expire with a visible error.
    void endpointRateTimeoutReportsError()
    {
        const QString dac = QStringLiteral("vela-test-timeout-dac");
        QVERIFY(control(QStringLiteral("add-sink vela-test-timeout-dac FL FR device.api=alsa")).startsWith(QLatin1String("ok")));
        QVERIFY(QTest::qWaitFor([&] { return !output(dac).isEmpty(); }, 5000));
        audio->setEndpointRate(QStringLiteral("output"), dac, 96000);
        QCOMPARE(output(dac).value("fixedRate").toInt(), 96000);
        QTRY_VERIFY_WITH_TIMEOUT(output(dac).value("rateError").toBool(), 10000);
        QCOMPARE(output(dac).value("fixedRate").toInt(), 0);
        audio->setEndpointRate(QStringLiteral("output"), dac, 0);
        QVERIFY(Config().rules().isEmpty());
        QCOMPARE(control(QStringLiteral("remove vela-test-timeout-dac")), QStringLiteral("ok"));
    }

    // One restart for changes in a row, never sooner than 1.5 s after a
    // change WirePlumber has to save; WirePlumber accepts the file.
    void restartsWaitForWirePlumber()
    {
        QVERIFY(QTest::qWaitFor([this] { return !audio->restartPending(); }, 5000)); // the last test's
        restarts.clear();
        restartTimes.clear();

        const QString dac = QStringLiteral("vela-test-dac");
        QVERIFY(control(QStringLiteral("add-sink vela-test-dac FL FR device.api=alsa")).startsWith(QLatin1String("ok")));
        QVERIFY(QTest::qWaitFor([&] { return !output(dac).isEmpty(); }, 5000));
        audio->setVolume(QStringLiteral("output"), speakers, 0.3);
        const qint64 changed = clock.elapsed();
        audio->setEndpointRate(QStringLiteral("output"), dac, 96000);
        QTest::qWait(200);
        audio->setEndpointFormat(QStringLiteral("output"), dac, QStringLiteral("F32LE"));
        QVERIFY(audio->restartPending());
        QVERIFY(QTest::qWaitFor([this] { return !restarts.isEmpty(); }, 5000));
        QCOMPARE(restarts, QStringList { QStringLiteral("wireplumber.service") });
        QVERIFY2(restartTimes.first() - changed >= 1450, qPrintable(QString::number(restartTimes.first() - changed)));
        QVERIFY(!audio->restartPending());
        QTest::qWait(500);
        QCOMPARE(restarts.size(), 1);

        // WirePlumber starts with Vela's file, and the volume was saved.
        QCOMPARE(control(QStringLiteral("restart-wireplumber")), QStringLiteral("ok"));
        QVERIFY(QTest::qWaitFor([this] { return audio->core()->snapshot().defaultsKnown && !output(speakers).isEmpty(); }, 10000));
        QVERIFY(std::abs(audio->core()->snapshot().endpoint(speakers)->volume() - 0.3) < 0.01);

        // "Restart audio" restarts PipeWire, which takes WirePlumber along.
        audio->restartAudio();
        QVERIFY(QTest::qWaitFor([this] { return restarts.size() == 4; }, 5000));
        QCOMPARE(restarts.mid(1), (QStringList {
            QStringLiteral("pipewire.service"),
            QStringLiteral("wireplumber.service"),
            QStringLiteral("pipewire-pulse.service"),
        }));

        QVERIFY(QTest::qWaitFor([&] { return !output(dac).isEmpty(); }, 10000));
        audio->setEndpointRate(QStringLiteral("output"), dac, 0);
        audio->setEndpointFormat(QStringLiteral("output"), dac, QString());
        QVERIFY(Config().rules().isEmpty());
        QCOMPARE(control(QStringLiteral("remove vela-test-dac")), QStringLiteral("ok"));
    }
};

QTEST_GUILESS_MAIN(ConfigTest)
#include "config.moc"
