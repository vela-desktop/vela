// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-audio's Audio (what the shell and Settings show) against a real
// PipeWire and WirePlumber (docs/audio.md §5.3, §5.4): lists, the grid and
// the 100% cap, values shown at once and the server winning afterwards,
// microphone actions only shown once confirmed, apps grouped by program,
// and the balance math.

#include "audio.h"
#include "core.h"
#include "harness.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QSignalSpy>

using namespace harness;
using vela::audio::AppIdentity;
using vela::audio::AppInfo;
using vela::audio::Audio;

namespace {

bool near(double a, double b)
{
    return std::abs(a - b) < 0.006;
}

QVariantMap find(const QVariantList& list, const QString& field, const QString& value)
{
    for (const QVariant& item : list) {
        if (item.toMap().value(field).toString() == value) {
            return item.toMap();
        }
    }
    return {};
}

// BlueZ, as far as a headset's battery goes (org.bluez.Battery1).
class FakeBlueZ : public QDBusVirtualObject {
public:
    int percent = 73;
    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
    {
        if (message.interface() != QLatin1String("org.freedesktop.DBus.Properties") || message.member() != QLatin1String("Get")
            || message.arguments().value(0).toString() != QLatin1String("org.bluez.Battery1")) {
            return false;
        }
        connection.send(message.createReply(QVariant::fromValue(QDBusVariant(QVariant::fromValue(uchar(percent))))));
        return true;
    }
};

} // namespace

class ViewTest : public QObject {
    Q_OBJECT

    std::unique_ptr<Audio> audio;
    // A private bus as the system bus, never the real one: BlueZ lives there.
    QProcess bus;
    FakeBlueZ bluez;

    QVariantMap output(const QString& name) const { return find(audio->outputs(), QStringLiteral("name"), name); }
    QVariantMap input(const QString& name) const { return find(audio->inputs(), QStringLiteral("name"), name); }
    QVariantMap app(const QString& key) const { return find(audio->apps(), QStringLiteral("key"), key); }
    quint32 id(const QString& name) const
    {
        const auto* e = audio->core()->snapshot().endpoint(name);
        return e ? e->id : 0;
    }
    // Straight from the server, without the interface's own values.
    double serverVolume(const QString& name) const
    {
        const auto* e = audio->core()->snapshot().endpoint(name);
        return e ? e->volume() : -1.0;
    }

private slots:
    void initTestCase()
    {
        bus.start(QStringLiteral("dbus-daemon"), { QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1") });
        QVERIFY2(bus.waitForStarted() && bus.waitForReadyRead(), "dbus-daemon is needed");
        const QString address = QString::fromUtf8(bus.readAllStandardOutput().trimmed());
        qputenv("DBUS_SYSTEM_BUS_ADDRESS", address.toUtf8());
        QDBusConnection fake = QDBusConnection::connectToBus(address, QStringLiteral("fake-bluez"));
        QVERIFY(fake.registerService(QStringLiteral("org.bluez")));
        QVERIFY(fake.registerVirtualObject(QStringLiteral("/org/bluez"), &bluez, QDBusConnection::SubPath));
    }

    void cleanupTestCase()
    {
        QDBusConnection::disconnectFromBus(QStringLiteral("fake-bluez"));
        bus.kill();
        bus.waitForFinished();
    }

    void init()
    {
        audio = std::make_unique<Audio>();
        QTRY_VERIFY_WITH_TIMEOUT(audio->available() && !output(speakers).isEmpty() && !input(microphone).isEmpty(), 5000);
        QTRY_COMPARE(audio->defaultOutput(), speakers);
    }

    void cleanup()
    {
        QTRY_VERIFY_WITH_TIMEOUT(audio->available() && id(speakers) && id(microphone), 10000);
        for (const QString& name : { speakers, headphones, surround, microphone }) {
            const QString node = QString::number(id(name));
            run(QStringLiteral("wpctl"), { QStringLiteral("set-volume"), node, QStringLiteral("1") });
            run(QStringLiteral("wpctl"), { QStringLiteral("set-mute"), node, QStringLiteral("0") });
        }
        run(QStringLiteral("wpctl"), { QStringLiteral("set-default"), QString::number(id(speakers)) });
        run(QStringLiteral("wpctl"), { QStringLiteral("set-default"), QString::number(id(microphone)) });
        audio.reset();
    }

    void balanceMath()
    {
        using vela::audio::balanceOf;
        using vela::audio::scaledChannels;
        QCOMPARE(scaledChannels({ 0.5, 0.25 }, 1.0), QList<double>({ 1.0, 0.5 }));
        QCOMPARE(scaledChannels({ 0.0, 0.0 }, 0.5), QList<double>({ 0.5, 0.5 }));
        QCOMPARE(scaledChannels({ 0.0, 0.0 }, 0.5, { 1.0, 0.5 }), QList<double>({ 0.5, 0.25 }));
        QCOMPARE(scaledChannels({ 0.5, 0.5 }, 1.7), QList<double>({ 1.0, 1.0 })); // the cap
        QCOMPARE(balanceOf({ 0.8, 0.4 }), QList<double>({ 1.0, 0.5 }));
        QVERIFY(balanceOf({ 0.0, 0.0 }).isEmpty());
    }

    void lists()
    {
        QCOMPARE(audio->outputs().size(), 3);
        QCOMPARE(audio->inputs().size(), 1);
        QVERIFY(output(speakers).value("isDefault").toBool());
        QVERIFY(!output(headphones).value("isDefault").toBool());
        QCOMPARE(output(speakers).value("description").toString(), QStringLiteral("Test Speakers"));
        QCOMPARE(output(surround).value("channels").toInt(), 6);
        QCOMPARE(output(speakers).value("intervals").toInt(), 65535); // continuous (volumesteps.h)
        QCOMPARE(audio->defaultInput(), microphone);
        QVERIFY(near(audio->volume(), 1.0));
        QVERIFY(!audio->muted());
    }

    void volumeOnTheGridAndCapped()
    {
        audio->setVolume(QStringLiteral("output"), speakers, 0.333);
        // Shown at once, before the server answers.
        QVERIFY(near(output(speakers).value("volume").toDouble(), 0.34));
        QVERIFY(near(audio->volume(), 0.34));
        QTRY_VERIFY(near(serverVolume(speakers), 0.34));

        // Above 100% from another program: shown as 100%, and the first step
        // down lands back on the grid.
        run(QStringLiteral("wpctl"), { QStringLiteral("set-volume"), QString::number(id(speakers)), QStringLiteral("1.3") });
        QTRY_VERIFY(near(serverVolume(speakers), 1.3));
        QTRY_VERIFY(near(audio->volume(), 1.0));
        audio->stepVolume(QStringLiteral("output"), speakers, -1);
        QTRY_VERIFY(near(serverVolume(speakers), 0.98));
        audio->setVolume(QStringLiteral("output"), speakers, 1.5);
        QTRY_VERIFY(near(serverVolume(speakers), 1.0));
    }

    void draggedSliderEndsOnTheLastValue()
    {
        QList<double> shown;
        connect(audio.get(), &Audio::changed, this, [&] { shown.append(audio->volume()); });
        for (int i = 1; i <= 20; i++) {
            audio->setVolume(QStringLiteral("output"), speakers, 1.0 - i * 0.02);
        }
        QTRY_VERIFY(near(serverVolume(speakers), 0.6));
        QTest::qWait(700); // past the optimistic window
        QVERIFY(near(audio->volume(), 0.6));
        // Once the drag ended, nothing older came back: only the last value
        // was written.
        const qsizetype end = shown.indexOf(0.6);
        QVERIFY(end >= 0);
        for (qsizetype i = end; i < shown.size(); i++) {
            QVERIFY2(near(shown[i], 0.6), qPrintable(QString::number(shown[i])));
        }
    }

    void theServerWinsAfterTheWindow()
    {
        audio->setVolume(QStringLiteral("output"), speakers, 0.5);
        QTRY_VERIFY(near(serverVolume(speakers), 0.5));
        // Another program sets it right after ours went out: shown once the
        // window passes.
        audio->setVolume(QStringLiteral("output"), speakers, 0.7);
        QTest::qWait(50);
        QVERIFY(near(audio->volume(), 0.7));
        run(QStringLiteral("wpctl"), { QStringLiteral("set-volume"), QString::number(id(speakers)), QStringLiteral("0.2") });
        QTRY_VERIFY_WITH_TIMEOUT(near(audio->volume(), 0.2), 3000);
    }

    void aStepUnmutesTheOutput()
    {
        audio->setMuted(QStringLiteral("output"), speakers, true);
        QVERIFY(audio->muted()); // at once
        QTRY_VERIFY(audio->core()->snapshot().endpoint(speakers)->muted);
        audio->stepVolume(QStringLiteral("output"), speakers, 1);
        QVERIFY(!audio->muted());
        QTRY_VERIFY(!audio->core()->snapshot().endpoint(speakers)->muted);
    }

    void microphoneMuteIsShownOnlyOnceConfirmed()
    {
        audio->setMuted(QStringLiteral("input"), microphone, true);
        QVERIFY(!input(microphone).value("muted").toBool());
        QVERIFY(input(microphone).value("mutePending").toBool());
        QVERIFY(!audio->inputMuted());
        QTRY_VERIFY(input(microphone).value("muted").toBool());
        QVERIFY(!input(microphone).value("mutePending").toBool());
        QVERIFY(!input(microphone).value("muteError").toBool());
        QVERIFY(audio->inputMuted());
    }

    void defaultInputIsShownOnlyOnceConfirmed()
    {
        QVERIFY(control(QStringLiteral("add-source vela-test-usb-mic")).startsWith(QLatin1String("ok")));
        QTRY_VERIFY(!input(QStringLiteral("vela-test-usb-mic")).isEmpty());
        audio->setDefault(QStringLiteral("input"), QStringLiteral("vela-test-usb-mic"));
        QCOMPARE(audio->defaultInput(), microphone);
        QVERIFY(input(microphone).value("defaultPending").toBool());
        // Not the real default yet: its mute can't be changed.
        audio->setMuted(QStringLiteral("input"), microphone, true);
        QVERIFY(!input(microphone).value("mutePending").toBool());
        QTRY_COMPARE(audio->defaultInput(), QStringLiteral("vela-test-usb-mic"));
        QVERIFY(!input(microphone).value("defaultPending").toBool());
        QVERIFY(input(QStringLiteral("vela-test-usb-mic")).value("isDefault").toBool());
        QCOMPARE(control(QStringLiteral("remove vela-test-usb-mic")), QStringLiteral("ok"));
        QTRY_COMPARE(audio->defaultInput(), microphone);
    }

    void defaultWithoutWirePlumberIsAnError()
    {
        QCOMPARE(control(QStringLiteral("stop-wireplumber")), QStringLiteral("ok"));
        QTRY_VERIFY(!audio->core()->snapshot().defaultsKnown);
        audio->setDefault(QStringLiteral("input"), microphone);
        QVERIFY(input(microphone).value("defaultError").toBool());
        QCOMPARE(control(QStringLiteral("start-wireplumber")), QStringLiteral("ok"));
        QTRY_COMPARE(audio->defaultInput(), microphone);
    }

    void outputDefaultIsShownAtOnce()
    {
        audio->setDefault(QStringLiteral("output"), headphones);
        QCOMPARE(audio->defaultOutput(), headphones);
        QVERIFY(output(headphones).value("isDefault").toBool());
        QTRY_COMPARE(audio->core()->snapshot().defaultOutput, headphones);
    }

    void appsGroupedByProgram()
    {
        audio->setAppResolver([](const AppIdentity& id) {
            return id.binary == QLatin1String("vela-player")
                ? AppInfo { QStringLiteral("org.vela.Player.desktop"), QStringLiteral("Vela Player"), QStringLiteral("vela-player-icon") }
                : AppInfo {};
        });
        auto tab1 = player(QStringLiteral("vela-player"));
        auto tab2 = player(QStringLiteral("vela-player"));
        auto voice = recorder(QStringLiteral("vela-player"));
        auto other = player(QStringLiteral("vela-other"));
        auto meter = std::make_unique<FakeStream>(QStringList { "--record", "--prop", "stream.monitor=true", "--prop",
            "stream.capture.sink=true", "--prop", "application.process.binary=vela-meter" });
        for (auto* s : { tab1.get(), tab2.get(), voice.get(), other.get(), meter.get() }) {
            QVERIFY(s->waitStreaming());
        }
        QTRY_VERIFY(!app(QStringLiteral("vela-player")).isEmpty() && !app(QStringLiteral("vela-other")).isEmpty());
        QTRY_COMPARE(audio->apps().size(), 2); // the meter isn't an app
        const QVariantMap a = app(QStringLiteral("vela-player"));
        QCOMPARE(a.value("name").toString(), QStringLiteral("Vela Player"));
        QCOMPARE(a.value("icon").toString(), QStringLiteral("vela-player-icon"));
        QCOMPARE(a.value("desktopId").toString(), QStringLiteral("org.vela.Player.desktop"));
        QVERIFY(a.value("playing").toBool());
        QVERIFY(a.value("recording").toBool());
        QVERIFY(!app(QStringLiteral("vela-other")).value("recording").toBool());
        QCOMPARE(app(QStringLiteral("vela-other")).value("name").toString(), QStringLiteral("vela-other"));
        QCOMPARE(audio->appKey(QStringLiteral("org.vela.Player.desktop")), QStringLiteral("vela-player"));
        QVERIFY(audio->appKey(QStringLiteral("nothing.desktop")).isEmpty());
    }

    void appVolumeMuteAndOutput()
    {
        auto tab1 = player(QStringLiteral("vela-player"));
        auto tab2 = player(QStringLiteral("vela-player"));
        const quint32 s1 = tab1->waitStreaming();
        const quint32 s2 = tab2->waitStreaming();
        QTRY_VERIFY(!app(QStringLiteral("vela-player")).isEmpty());
        const auto streamVolume = [&](quint32 s) {
            const auto* st = audio->core()->snapshot().stream(s);
            return st ? st->volume() : -1.0;
        };

        audio->setAppVolume(QStringLiteral("vela-player"), 0.51);
        QVERIFY(near(app(QStringLiteral("vela-player")).value("volume").toDouble(), 0.52)); // the 2-point grid
        QTRY_VERIFY(near(streamVolume(s1), 0.52) && near(streamVolume(s2), 0.52));
        audio->stepAppVolume(QStringLiteral("vela-player"), -1);
        QTRY_VERIFY(near(streamVolume(s1), 0.5) && near(streamVolume(s2), 0.5));

        audio->setAppMuted(QStringLiteral("vela-player"), true);
        QVERIFY(app(QStringLiteral("vela-player")).value("muted").toBool());
        QTRY_VERIFY(audio->core()->snapshot().stream(s1)->muted && audio->core()->snapshot().stream(s2)->muted);

        audio->setAppOutput(QStringLiteral("vela-player"), headphones);
        QCOMPARE(app(QStringLiteral("vela-player")).value("output").toString(), headphones);
        QCOMPARE(app(QStringLiteral("vela-player")).value("outputDescription").toString(), QStringLiteral("Test Headphones"));
        QTRY_VERIFY(audio->core()->snapshot().stream(s1)->endpoint == id(headphones)
            && audio->core()->snapshot().stream(s2)->endpoint == id(headphones));

        audio->resetApps();
        QVERIFY(app(QStringLiteral("vela-player")).value("output").toString().isEmpty());
        QTRY_VERIFY(audio->core()->snapshot().stream(s1)->endpoint == id(speakers)
            && audio->core()->snapshot().stream(s2)->endpoint == id(speakers));
        QTest::qWait(700);
        QVERIFY(app(QStringLiteral("vela-player")).value("output").toString().isEmpty());
    }

    void appInput()
    {
        QVERIFY(control(QStringLiteral("add-source vela-test-usb-mic")).startsWith(QLatin1String("ok")));
        auto voice = recorder(QStringLiteral("vela-caller"));
        const quint32 s = voice->waitStreaming();
        QTRY_VERIFY(app(QStringLiteral("vela-caller")).value("recording").toBool());
        QVERIFY(!app(QStringLiteral("vela-caller")).value("playing").toBool());
        audio->setAppInput(QStringLiteral("vela-caller"), QStringLiteral("vela-test-usb-mic"));
        QTRY_COMPARE(audio->core()->snapshot().stream(s)->endpoint, id(QStringLiteral("vela-test-usb-mic")));
        QCOMPARE(app(QStringLiteral("vela-caller")).value("input").toString(), QStringLiteral("vela-test-usb-mic"));
        audio->setAppInput(QStringLiteral("vela-caller"), QString());
        QTRY_COMPARE(audio->core()->snapshot().stream(s)->endpoint, id(microphone));
        QCOMPARE(voice->close(), 0);
        QCOMPARE(control(QStringLiteral("remove vela-test-usb-mic")), QStringLiteral("ok"));
    }

    void bluetoothBattery()
    {
        bluez.percent = 73;
        QVERIFY(control(QStringLiteral("add-sink vela-test-headset FL FR api.bluez5.address=AA:BB:CC:DD:EE:FF")).startsWith(QLatin1String("ok")));
        QTRY_VERIFY(!output(QStringLiteral("vela-test-headset")).isEmpty());
        QTRY_COMPARE(output(QStringLiteral("vela-test-headset")).value("battery").toInt(), 73);
        QCOMPARE(output(speakers).value("battery").toInt(), -1);

        // BlueZ says it changed: followed without asking again.
        QDBusMessage changed = QDBusMessage::createSignal(QStringLiteral("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
        changed << QStringLiteral("org.bluez.Battery1") << QVariantMap { { QStringLiteral("Percentage"), QVariant::fromValue(uchar(40)) } }
                << QStringList();
        QVERIFY(QDBusConnection(QStringLiteral("fake-bluez")).send(changed));
        QTRY_COMPARE(output(QStringLiteral("vela-test-headset")).value("battery").toInt(), 40);
        QCOMPARE(control(QStringLiteral("remove vela-test-headset")), QStringLiteral("ok"));
    }

    void availableFollowsTheConnection()
    {
        QSignalSpy spy(audio.get(), &Audio::availableChanged);
        QCOMPARE(control(QStringLiteral("stop-pipewire")), QStringLiteral("ok"));
        QTRY_VERIFY(!audio->available());
        QTRY_VERIFY(audio->outputs().isEmpty());
        audio->setVolume(QStringLiteral("output"), speakers, 0.5); // nothing to do, no crash
        QCOMPARE(control(QStringLiteral("start-pipewire")), QStringLiteral("ok"));
        QTRY_VERIFY_WITH_TIMEOUT(audio->available() && !output(speakers).isEmpty(), 10000);
        QVERIFY(spy.size() >= 2);
    }
};

QTEST_GUILESS_MAIN(ViewTest)
#include "view.moc"
