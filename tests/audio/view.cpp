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
#include "levelmeter.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QSignalSpy>
#include <QTimer>

using namespace harness;
using vela::audio::AppIdentity;
using vela::audio::AppInfo;
using vela::audio::Audio;
using vela::audio::LevelMeter;

namespace {

bool near(double a, double b)
{
    return std::abs(a - b) < 0.006;
}

bool near(const QList<double>& a, const QList<double>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (qsizetype i = 0; i < a.size(); i++) {
        if (!near(a[i], b[i])) {
            return false;
        }
    }
    return true;
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
    int delayMs = 0;
    int getRequests = 0;
    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override
    {
        if (message.interface() != QLatin1String("org.freedesktop.DBus.Properties") || message.member() != QLatin1String("Get")
            || message.arguments().value(0).toString() != QLatin1String("org.bluez.Battery1")) {
            return false;
        }
        ++getRequests;
        const int value = percent; // snapshot the older response
        if (delayMs > 0) {
            message.setDelayedReply(true);
            QTimer::singleShot(delayMs, this, [message, connection, value] {
                connection.send(message.createReply(QVariant::fromValue(QDBusVariant(QVariant::fromValue(uchar(value))))));
            });
        } else {
            connection.send(message.createReply(QVariant::fromValue(QDBusVariant(QVariant::fromValue(uchar(value))))));
        }
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
    QVariantMap device(const QString& name) const { return find(audio->devices(), QStringLiteral("name"), name); }
    QStringList profileNames(const QString& name) const
    {
        QStringList out;
        for (const QVariant& p : device(name).value(QStringLiteral("profiles")).toList()) {
            out.append(p.toMap().value(QStringLiteral("name")).toString());
        }
        return out;
    }
    QList<double> serverChannels(const QString& name) const
    {
        const auto* e = audio->core()->snapshot().endpoint(name);
        return e ? e->channelVolumes : QList<double>();
    }
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

    QStringList microphoneKeys() const
    {
        QStringList out;
        for (const QVariant& user : audio->microphoneUsers()) {
            out.append(user.toMap().value(QStringLiteral("key")).toString());
        }
        return out;
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
        bluez.percent = 73;
        bluez.delayMs = 0;
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

        // Left and right (PulseAudio's balance).
        using vela::audio::hasStereo;
        using vela::audio::stereoBalance;
        using vela::audio::withStereoBalance;
        const QStringList lr { QStringLiteral("FL"), QStringLiteral("FR") };
        QVERIFY(near(stereoBalance(lr, { 1.0, 0.5 }), -0.5));
        QVERIFY(near(stereoBalance(lr, { 0.5, 1.0 }), 0.5));
        QVERIFY(near(stereoBalance(lr, { 0.7, 0.7 }), 0.0));
        QVERIFY(near(stereoBalance(lr, { 0.0, 0.0 }), 0.0));
        QVERIFY(near(withStereoBalance(lr, { 0.8, 0.8 }, 0.5), { 0.4, 0.8 }));
        QVERIFY(near(withStereoBalance(lr, { 0.8, 0.4 }, 0.0), { 0.8, 0.8 }));
        QVERIFY(near(withStereoBalance(lr, { 0.8, 0.8 }, -1.0), { 0.8, 0.0 }));
        // Center and LFE are left alone; rear channels follow their side.
        const QStringList surround { QStringLiteral("FL"), QStringLiteral("FR"), QStringLiteral("FC"),
            QStringLiteral("LFE"), QStringLiteral("RL"), QStringLiteral("RR") };
        QVERIFY(near(withStereoBalance(surround, { 1, 1, 0.5, 0.5, 1, 1 }, -1.0), { 1, 0, 0.5, 0.5, 1, 0 }));
        QVERIFY(near(stereoBalance(surround, { 1, 0.5, 0.2, 0.2, 1, 0.5 }), -0.5));
        QVERIFY(hasStereo(lr));
        QVERIFY(!hasStereo({ QStringLiteral("MONO") }));
        QVERIFY(!hasStereo({ QStringLiteral("FC"), QStringLiteral("LFE") }));
    }

    void balanceKeepsTheVolume()
    {
        QVERIFY(output(speakers).value("stereo").toBool());
        QVERIFY(!input(microphone).value("stereo").toBool());
        audio->setBalance(QStringLiteral("output"), speakers, 0.5);
        QVERIFY(near(output(speakers).value("balance").toDouble(), 0.5)); // at once
        QTRY_VERIFY(near(serverChannels(speakers), { 0.5, 1.0 }));
        QVERIFY(near(audio->volume(), 1.0));

        audio->setVolume(QStringLiteral("output"), speakers, 0.5);
        QTRY_VERIFY(near(serverChannels(speakers), { 0.25, 0.5 }));
        QTest::qWait(600); // past the optimistic window: the server's value
        QVERIFY(near(output(speakers).value("balance").toDouble(), 0.5));

        // Both at once, before either reaches the server: neither is lost.
        audio->setBalance(QStringLiteral("output"), speakers, -0.5);
        audio->setVolume(QStringLiteral("output"), speakers, 0.8);
        QTRY_VERIFY(near(serverChannels(speakers), { 0.8, 0.4 }));

        // From silence, the balance comes back.
        audio->setVolume(QStringLiteral("output"), speakers, 0.0);
        QTRY_VERIFY(near(serverChannels(speakers), { 0.0, 0.0 }));
        QTest::qWait(600);
        QVERIFY(near(output(speakers).value("balance").toDouble(), -0.5));
        audio->setVolume(QStringLiteral("output"), speakers, 0.6);
        QTRY_VERIFY(near(serverChannels(speakers), { 0.6, 0.3 }));

        // A mono input has no balance.
        const QList<double> before = serverChannels(microphone);
        audio->setBalance(QStringLiteral("input"), microphone, 0.5);
        QVERIFY(near(input(microphone).value("balance").toDouble(), 0.0));
        QTest::qWait(100);
        QVERIFY(near(serverChannels(microphone), before));
    }

    void devicesAndProfiles()
    {
        FakeDevice card;
        const quint32 cardId = card.waitReady();
        QVERIFY(cardId);
        QTRY_VERIFY(!device(QStringLiteral("vela-test-card")).isEmpty());
        const QVariantMap d = device(QStringLiteral("vela-test-card"));
        QCOMPARE(d.value("description").toString(), QStringLiteral("Test Card"));
        QCOMPARE(d.value("profile").toInt(), 1);
        QVERIFY(!d.value("off").toBool());
        QVERIFY(!d.value("bluetooth").toBool());
        // HDMI: nothing plugged in, not offered.
        QCOMPARE(profileNames(QStringLiteral("vela-test-card")),
            QStringList({ QStringLiteral("off"), QStringLiteral("output:analog-stereo"), QStringLiteral("a2dp-sink-ldac") }));
        card.send(QStringLiteral("available 2 yes"));
        QTRY_COMPARE(profileNames(QStringLiteral("vela-test-card")).size(), 4);

        // An output of the card says so, and the card lists it.
        QVERIFY(control(QStringLiteral("add-sink vela-test-card-output FL FR device.id=%1").arg(cardId)).startsWith(QLatin1String("ok")));
        QTRY_COMPARE(output(QStringLiteral("vela-test-card-output")).value("device").toString(), QStringLiteral("vela-test-card"));
        QCOMPARE(device(QStringLiteral("vela-test-card")).value("outputs").toStringList(), QStringList({ QStringLiteral("vela-test-card-output") }));
        QVERIFY(output(speakers).value("device").toString().isEmpty());
        QCOMPARE(control(QStringLiteral("remove vela-test-card-output")), QStringLiteral("ok"));
        QCOMPARE(card.close(), 0);
        QTRY_VERIFY(device(QStringLiteral("vela-test-card")).isEmpty());
    }

    void profileChosenAndRemembered()
    {
        FakeDevice card;
        QVERIFY(card.waitReady());
        QTRY_VERIFY(!device(QStringLiteral("vela-test-card")).isEmpty());

        audio->setProfile(QStringLiteral("vela-test-card"), 3);
        QCOMPARE(device(QStringLiteral("vela-test-card")).value("profile").toInt(), 3); // at once
        QVERIFY(device(QStringLiteral("vela-test-card")).value("profilePending").toBool());
        // Saved: the user chose it.
        QTRY_VERIFY(card.sets().contains(qMakePair(3, true)));
        QTRY_VERIFY(!device(QStringLiteral("vela-test-card")).value("profilePending").toBool());
        QCOMPARE(audio->core()->snapshot().device(QStringLiteral("vela-test-card"))->profile, 3);

        // Off, and on again.
        audio->setProfile(QStringLiteral("vela-test-card"), 0);
        QVERIFY(device(QStringLiteral("vela-test-card")).value("off").toBool());
        QTRY_VERIFY(audio->core()->snapshot().device(QStringLiteral("vela-test-card"))->off());
        audio->setProfile(QStringLiteral("vela-test-card"), 3);
        QTRY_COMPARE(audio->core()->snapshot().device(QStringLiteral("vela-test-card"))->profile, 3);

        // WirePlumber keeps the choice across a restart.
        QTest::qWait(wireplumberSaveDelayMs);
        QCOMPARE(control(QStringLiteral("restart-wireplumber")), QStringLiteral("ok"));
        QTRY_VERIFY_WITH_TIMEOUT(audio->core()->snapshot().defaultsKnown, 5000);
        QTest::qWait(500);
        QCOMPARE(device(QStringLiteral("vela-test-card")).value("profile").toInt(), 3);

        // An index the card doesn't have.
        audio->setProfile(QStringLiteral("vela-test-card"), 99);
        QVERIFY(device(QStringLiteral("vela-test-card")).value("profileError").toBool());
        QCOMPARE(card.close(), 0);
    }

    void optimisticProfileTimeoutReportsError()
    {
        FakeDevice card;
        QVERIFY(card.waitReady());
        QTRY_VERIFY(!device(QStringLiteral("vela-test-card")).isEmpty());
        // WirePlumber may restore the previous test's saved profile. Always
        // choose a different one, or the request needs no server update.
        const int original = device(QStringLiteral("vela-test-card")).value("profile").toInt();
        const int requested = original == 3 ? 1 : 3;
        QVERIFY(card.ignoreProfileChanges(true));
        audio->setProfile(QStringLiteral("vela-test-card"), requested);
        QCOMPARE(device(QStringLiteral("vela-test-card")).value("profile").toInt(), requested); // optimistic
        QVERIFY(device(QStringLiteral("vela-test-card")).value("profilePending").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(device(QStringLiteral("vela-test-card")).value("profileError").toBool(), 5000);
        QCOMPARE(device(QStringLiteral("vela-test-card")).value("profile").toInt(), original); // server won

        // A subsequent, confirmed request clears the previous timeout error.
        QVERIFY(card.ignoreProfileChanges(false));
        audio->setProfile(QStringLiteral("vela-test-card"), requested);
        QTRY_COMPARE(device(QStringLiteral("vela-test-card")).value("profile").toInt(), requested);
        QTRY_VERIFY(!device(QStringLiteral("vela-test-card")).value("profilePending").toBool());
        QVERIFY(!device(QStringLiteral("vela-test-card")).value("profileError").toBool());
        QCOMPARE(card.close(), 0);
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

    void recoveredDefaultDoesNotBlockMicrophone()
    {
        QCOMPARE(control(QStringLiteral("stop-wireplumber")), QStringLiteral("ok"));
        QTRY_VERIFY(!audio->core()->snapshot().defaultsKnown);
        audio->setDefault(QStringLiteral("input"), microphone);
        QVERIFY(input(microphone).value("defaultError").toBool());

        QCOMPARE(control(QStringLiteral("start-wireplumber")), QStringLiteral("ok"));
        QTRY_VERIFY(audio->core()->snapshot().defaultsKnown);
        QTRY_COMPARE(audio->defaultInput(), microphone);
        QVERIFY(!input(microphone).value("defaultError").toBool());

        audio->setMuted(QStringLiteral("input"), microphone, true);
        QTRY_VERIFY(input(microphone).value("muted").toBool());
        audio->setVolume(QStringLiteral("input"), microphone, 0.6);
        QTRY_VERIFY(near(serverVolume(microphone), 0.6));
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

    void microphoneInUse()
    {
        audio->setAppResolver([](const AppIdentity& id) {
            return id.binary == QLatin1String("vela-caller")
                ? AppInfo { QStringLiteral("org.vela.Caller.desktop"), QStringLiteral("Vela Caller"), QStringLiteral("vela-caller-icon") }
                : AppInfo {};
        });
        QVERIFY(audio->microphoneUsers().isEmpty());
        auto caller = recorder(QStringLiteral("vela-caller"));
        QVERIFY(caller->waitStreaming());
        QTRY_COMPARE(microphoneKeys(), QStringList({ QStringLiteral("vela-caller") }));
        const QVariantMap user = audio->microphoneUsers().first().toMap();
        QCOMPARE(user.value("name").toString(), QStringLiteral("Vela Caller"));
        QCOMPARE(user.value("icon").toString(), QStringLiteral("vela-caller-icon"));
        QCOMPARE(user.value("desktopId").toString(), QStringLiteral("org.vela.Caller.desktop"));

        // Paused, it stops counting, but only after a moment: a pause
        // doesn't make the icon blink.
        caller->send(QStringLiteral("pause"));
        QTRY_VERIFY(audio->core()->snapshot().stream(caller->node()) && !audio->core()->snapshot().stream(caller->node())->running);
        QCOMPARE(microphoneKeys(), QStringList({ QStringLiteral("vela-caller") }));
        QTRY_VERIFY_WITH_TIMEOUT(audio->microphoneUsers().isEmpty(), 2000);
        caller->send(QStringLiteral("resume"));
        QTRY_COMPARE(microphoneKeys(), QStringList({ QStringLiteral("vela-caller") }));
        // Gone for good: the same.
        QCOMPARE(caller->close(), 0);
        QTest::qWait(200);
        QCOMPARE(microphoneKeys(), QStringList({ QStringLiteral("vela-caller") }));
        QTRY_VERIFY_WITH_TIMEOUT(audio->microphoneUsers().isEmpty(), 2000);
    }

    void whatIsNotTheMicrophone()
    {
        // A screen recorder taking the desktop's sound, and meters on
        // outputs or apps: none of them listen to a microphone source.
        auto screen = std::make_unique<FakeStream>(QStringList { "--record", "--prop", "stream.capture.sink=true", "--prop",
            "target.object=" + speakers, "--prop", "application.process.binary=vela-screen-recorder" });
        auto music = player(QStringLiteral("vela-player"), { QStringLiteral("--amplitude"), QStringLiteral("0.2") });
        for (auto* s : { screen.get(), music.get() }) {
            QVERIFY(s->waitStreaming());
        }
        LevelMeter outputMeter;
        outputMeter.setAudio(audio.get());
        outputMeter.setKind(QStringLiteral("output"));
        outputMeter.setName(speakers);
        outputMeter.setActive(true);
        LevelMeter appMeter;
        appMeter.setAudio(audio.get());
        appMeter.setKind(QStringLiteral("app"));
        appMeter.setName(QStringLiteral("vela-player"));
        appMeter.setActive(true);
        QTRY_VERIFY(!outputMeter.monitored().isEmpty() && !appMeter.monitored().isEmpty());
        QTRY_VERIFY(std::count_if(audio->core()->snapshot().streams.begin(), audio->core()->snapshot().streams.end(),
                        [](const vela::audio::Stream& s) { return s.monitor && s.running; })
            == 2);
        QTest::qWait(300);
        QVERIFY2(audio->microphoneUsers().isEmpty(), qPrintable(microphoneKeys().join(u' ')));

        // An input's meter (Settings' Sound page) does listen: it counts.
        LevelMeter inputMeter;
        inputMeter.setAudio(audio.get());
        inputMeter.setKind(QStringLiteral("input"));
        inputMeter.setName(microphone);
        inputMeter.setActive(true);
        QTRY_COMPARE(microphoneKeys(), QStringList({ QCoreApplication::applicationName() }));
        inputMeter.setActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(audio->microphoneUsers().isEmpty(), 2000);
    }

    void clientPropertiesCannotHideMicrophoneRecording()
    {
        // These flags are supplied by the client, not by a trusted
        // authority. A recording stream with an actual source link must
        // remain visible even when it claims to be an internal stream
        // or a monitor.
        const QStringList spoofed {
            QStringLiteral("node.link-group=vela-test-internal"),
            QStringLiteral("node.virtual=true"),
            QStringLiteral("stream.monitor=true"),
        };
        for (qsizetype i = 0; i < spoofed.size(); ++i) {
            const QString key = QStringLiteral("vela-spoof-%1").arg(i);
            auto client = recorder(key, {
                QStringLiteral("--prop"), spoofed[i],
                QStringLiteral("--prop"), QStringLiteral("target.object=") + microphone,
            });
            const quint32 node = client->waitStreaming();
            QVERIFY2(node != 0, qPrintable(spoofed[i]));
            QTRY_VERIFY_WITH_TIMEOUT(audio->core()->snapshot().stream(node)
                && audio->core()->snapshot().stream(node)->endpoint == id(microphone), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(microphoneKeys().contains(key), 5000);
            QCOMPARE(client->close(), 0);
            QTRY_VERIFY_WITH_TIMEOUT(!microphoneKeys().contains(key), 2000);
        }
    }

    void aVirtualSourceIsAMicrophone()
    {
        // Like an echo canceller's: what records it uses the microphone.
        QVERIFY(control(QStringLiteral("add-source vela-test-echo-cancel")).startsWith(QLatin1String("ok")));
        QTRY_VERIFY(!input(QStringLiteral("vela-test-echo-cancel")).isEmpty());
        auto caller = recorder(QStringLiteral("vela-caller"),
            { QStringLiteral("--prop"), QStringLiteral("target.object=vela-test-echo-cancel") });
        const quint32 s = caller->waitStreaming();
        QVERIFY(s);
        QTRY_VERIFY(audio->core()->snapshot().stream(s)
            && audio->core()->snapshot().stream(s)->endpoint == id(QStringLiteral("vela-test-echo-cancel")));
        QTRY_COMPARE(microphoneKeys(), QStringList({ QStringLiteral("vela-caller") }));
        QCOMPARE(caller->close(), 0);
        QCOMPARE(control(QStringLiteral("remove vela-test-echo-cancel")), QStringLiteral("ok"));
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

    void bluetoothLateGetCannotOverwriteChangedValue()
    {
        bluez.percent = 73;
        bluez.delayMs = 300;
        const int before = bluez.getRequests;
        QVERIFY(control(QStringLiteral("add-sink vela-test-headset FL FR api.bluez5.address=AA:BB:CC:DD:EE:FF")).startsWith(QLatin1String("ok")));
        QTRY_VERIFY(!output(QStringLiteral("vela-test-headset")).isEmpty());
        QTRY_VERIFY(bluez.getRequests > before); // the initial Get is in flight

        QDBusMessage changed = QDBusMessage::createSignal(QStringLiteral("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
        changed << QStringLiteral("org.bluez.Battery1")
                << QVariantMap { { QStringLiteral("Percentage"), QVariant::fromValue(uchar(40)) } } << QStringList();
        QVERIFY(QDBusConnection(QStringLiteral("fake-bluez")).send(changed));
        QTRY_COMPARE(output(QStringLiteral("vela-test-headset")).value("battery").toInt(), 40);
        QTest::qWait(400); // the late Get returning 73 must not roll back 40
        QCOMPARE(output(QStringLiteral("vela-test-headset")).value("battery").toInt(), 40);
        QCOMPARE(control(QStringLiteral("remove vela-test-headset")), QStringLiteral("ok"));
    }

    void bluetoothReconnectQueriesBatteryAgain()
    {
        bluez.percent = 73;
        const int before = bluez.getRequests;
        const QString add = QStringLiteral("add-sink vela-test-headset FL FR api.bluez5.address=AA:BB:CC:DD:EE:FF");
        QVERIFY(control(add).startsWith(QLatin1String("ok")));
        QTRY_COMPARE(output(QStringLiteral("vela-test-headset")).value("battery").toInt(), 73);
        QCOMPARE(control(QStringLiteral("remove vela-test-headset")), QStringLiteral("ok"));
        QTRY_VERIFY(output(QStringLiteral("vela-test-headset")).isEmpty());

        bluez.percent = 29;
        QVERIFY(control(add).startsWith(QLatin1String("ok")));
        QTRY_COMPARE(output(QStringLiteral("vela-test-headset")).value("battery").toInt(), 29);
        QVERIFY(bluez.getRequests >= before + 2);
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
