// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-audio's Core against a real PipeWire and WirePlumber (the fixture,
// docs/audio.md §11): the model, changes from outside, actions, the 100%
// cap, balance, streams and their targets, devices appearing, and restarts.

#include "core.h"
#include "harness.h"

#include <QSignalSpy>

using namespace harness;
using vela::audio::Core;
using vela::audio::Direction;
using vela::audio::Endpoint;
using vela::audio::Stream;

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

} // namespace

class CoreTest : public QObject {
    Q_OBJECT

    std::unique_ptr<Core> core;

    const Endpoint* endpoint(const QString& name) const { return core->snapshot().endpoint(name); }
    const Stream* stream(quint32 id) const { return core->snapshot().stream(id); }
    QString linkedTo(quint32 id) const { return harness::linkedTo(*core, id); }
    QString targetOf(quint32 id) const { return harness::targetOf(*core, id); }

private slots:
    void init()
    {
        core = std::make_unique<Core>();
        QTRY_VERIFY_WITH_TIMEOUT(core->connected() && core->snapshot().defaultsKnown, 5000);
        QTRY_VERIFY(endpoint(speakers) && endpoint(microphone));
    }

    void cleanup()
    {
        // Every test starts from the fixture's own state.
        if (!core->connected()) {
            core = std::make_unique<Core>();
            QTRY_VERIFY_WITH_TIMEOUT(core->connected() && endpoint(speakers) && endpoint(microphone), 10000);
        }
        const auto id = [&](const QString& name) { return QString::number(endpoint(name) ? endpoint(name)->id : 0); };
        run(QStringLiteral("wpctl"), { QStringLiteral("set-default"), id(speakers) });
        run(QStringLiteral("wpctl"), { QStringLiteral("set-default"), id(microphone) });
        for (const QString& name : { speakers, headphones, surround, microphone }) {
            run(QStringLiteral("wpctl"), { QStringLiteral("set-volume"), id(name), QStringLiteral("1") });
            run(QStringLiteral("wpctl"), { QStringLiteral("set-mute"), id(name), QStringLiteral("0") });
        }
        core.reset();
    }

    void builtInPipewireRatesDoNotMaskUserConfiguration()
    {
        using vela::audio::Snapshot;
        using vela::audio::hasBuiltInPipewireRates;

        Snapshot s;
        s.settingsKnown = true;
        s.clockRate = 48000;
        s.allowedRates = {};
        QVERIFY(hasBuiltInPipewireRates(s));
        s.allowedRates = { 48000 };
        QVERIFY(hasBuiltInPipewireRates(s));

        // A user-configured clock is not "nobody configured it", even
        // without an allowed-rates list or Vela's own config file.
        s.clockRate = 96000;
        s.allowedRates = {};
        QVERIFY(!hasBuiltInPipewireRates(s));
        s.allowedRates = { 96000 };
        QVERIFY(!hasBuiltInPipewireRates(s));
        s.clockRate = 48000;
        QVERIFY(!hasBuiltInPipewireRates(s)); // non-default allowed rates
        s.settingsKnown = false;
        s.allowedRates = { 48000 };
        QVERIFY(!hasBuiltInPipewireRates(s));
    }

    void endpointsAndDefaults()
    {
        const auto& s = core->snapshot();
        QCOMPARE(s.defaultOutput, speakers);
        QCOMPARE(s.defaultInput, microphone);
        int outputs = 0;
        for (const Endpoint& e : s.endpoints) {
            outputs += e.direction == Direction::Output;
        }
        QCOMPARE(outputs, 3);
        QCOMPARE(endpoint(speakers)->description, QStringLiteral("Test Speakers"));
        QCOMPARE(endpoint(speakers)->positions, QStringList({ "FL", "FR" }));
        QCOMPARE(endpoint(surround)->positions, QStringList({ "FL", "FR", "FC", "LFE", "RL", "RR" }));
        QCOMPARE(endpoint(speakers)->icon, QStringLiteral("audio-speakers"));
        QCOMPARE(endpoint(microphone)->icon, QStringLiteral("audio-input-microphone"));
        QCOMPARE(endpoint(microphone)->direction, Direction::Input);
        QVERIFY(endpoint(speakers)->isDefault);
        QVERIFY(!endpoint(headphones)->isDefault);
        QVERIFY(endpoint(speakers)->available);
        QVERIFY(near(endpoint(speakers)->volume(), 1.0));
    }

    void changesFromOutsideArriveAsEvents()
    {
        const QString id = QString::number(endpoint(speakers)->id);
        run(QStringLiteral("wpctl"), { QStringLiteral("set-volume"), id, QStringLiteral("0.5") });
        QTRY_VERIFY(near(endpoint(speakers)->volume(), 0.5));
        run(QStringLiteral("wpctl"), { QStringLiteral("set-mute"), id, QStringLiteral("1") });
        QTRY_VERIFY(endpoint(speakers)->muted);
        run(QStringLiteral("wpctl"), { QStringLiteral("set-default"), QString::number(endpoint(headphones)->id) });
        QTRY_COMPARE(core->snapshot().defaultOutput, headphones);
        QVERIFY(endpoint(headphones)->isDefault);
    }

    void volumeIsCappedAt100()
    {
        const quint32 id = endpoint(speakers)->id;
        QVERIFY(core->setVolume(id, 0.4));
        QTRY_VERIFY(near(endpoint(speakers)->volume(), 0.4));
        QVERIFY(core->setVolume(id, 1.3));
        QTRY_VERIFY(near(endpoint(speakers)->volume(), 1.0));
        // Another program may go higher: the model says so.
        run(QStringLiteral("wpctl"), { QStringLiteral("set-volume"), QString::number(id), QStringLiteral("1.3") });
        QTRY_VERIFY(near(endpoint(speakers)->volume(), 1.3));
    }

    void balanceIsKept()
    {
        const quint32 id = endpoint(speakers)->id;
        QVERIFY(core->setChannelVolumes(id, { 0.5, 0.25 }));
        QTRY_VERIFY(near(endpoint(speakers)->channelVolumes, { 0.5, 0.25 }));
        QVERIFY(core->setVolume(id, 0.8));
        QTRY_VERIFY(near(endpoint(speakers)->channelVolumes, { 0.8, 0.4 }));
        // Through silence and back: the same shape.
        QVERIFY(core->setVolume(id, 0.0));
        QTRY_VERIFY(near(endpoint(speakers)->channelVolumes, { 0.0, 0.0 }));
        QVERIFY(core->setVolume(id, 0.6));
        QTRY_VERIFY(near(endpoint(speakers)->channelVolumes, { 0.6, 0.3 }));
    }

    void muteAndDefault()
    {
        QVERIFY(core->setMuted(endpoint(microphone)->id, true));
        QTRY_VERIFY(endpoint(microphone)->muted);
        QVERIFY(core->setMuted(endpoint(microphone)->id, false));
        QTRY_VERIFY(!endpoint(microphone)->muted);
        QVERIFY(core->setDefault(Direction::Output, headphones));
        QTRY_COMPARE(core->snapshot().defaultOutput, headphones);
        QVERIFY(!core->setDefault(Direction::Input, headphones)); // not an input
        QVERIFY(!core->setDefault(Direction::Output, QStringLiteral("no-such-output")));
    }

    void streams()
    {
        auto music = player(QStringLiteral("vela-player"), { QStringLiteral("--prop"), QStringLiteral("media.role=Music") });
        auto voice = recorder(QStringLiteral("vela-recorder"));
        auto meter = std::make_unique<FakeStream>(QStringList { "--record", "--prop", "stream.monitor=true", "--prop",
            "stream.capture.sink=true", "--prop", "target.object=" + speakers });
        const quint32 musicId = music->waitStreaming();
        const quint32 voiceId = voice->waitStreaming();
        const quint32 meterId = meter->waitStreaming();
        QVERIFY(musicId && voiceId && meterId);

        QTRY_COMPARE(linkedTo(musicId), speakers);
        const Stream* m = stream(musicId);
        QCOMPARE(m->direction, Direction::Output);
        QCOMPARE(m->app.key, QStringLiteral("vela-player"));
        QCOMPARE(m->app.binary, QStringLiteral("vela-player"));
        QCOMPARE(m->mediaRole, QStringLiteral("Music"));
        QVERIFY(m->target.isEmpty());
        QVERIFY(m->isApp());
        QTRY_VERIFY(stream(musicId) && stream(musicId)->running);

        QTRY_COMPARE(linkedTo(voiceId), microphone);
        QCOMPARE(stream(voiceId)->direction, Direction::Input);
        QVERIFY(stream(voiceId)->isApp());

        QTRY_VERIFY(stream(meterId) && stream(meterId)->monitor);
        QVERIFY(stream(meterId)->capturesOutput);
        QVERIFY(!stream(meterId)->isApp());

        // A stream's own volume, equal on every channel.
        QVERIFY(core->setVolume(musicId, 0.3));
        QTRY_VERIFY(stream(musicId) && near(stream(musicId)->channelVolumes, { 0.3, 0.3 }));
        QVERIFY(core->setMuted(musicId, true));
        QTRY_VERIFY(stream(musicId) && stream(musicId)->muted);

        QCOMPARE(music->close(), 0);
        QTRY_VERIFY(!stream(musicId));
        QVERIFY(core->connected());
        // Missing streams must produce actionable failures, never null dereferences.
        const QString missing = QStringLiteral("(stream %1 is not in the snapshot)").arg(musicId);
        QCOMPARE(linkedTo(musicId), missing);
        QCOMPARE(targetOf(musicId), missing);
    }

    void outputChosenForAStream()
    {
        auto music = player(QStringLiteral("vela-mover"));
        const quint32 id = music->waitStreaming();
        QTRY_COMPARE(linkedTo(id), speakers);

        QVERIFY(core->setTarget(id, headphones));
        QTRY_COMPARE(targetOf(id), headphones);
        QTRY_COMPARE(linkedTo(id), headphones);
        QVERIFY(!core->setTarget(id, microphone)); // not an output

        // The default one again: it follows the default output.
        QVERIFY(core->setTarget(id, QString()));
        QTRY_COMPARE(targetOf(id), QString());
        QTRY_COMPARE(linkedTo(id), speakers);
    }

    void chosenOutputIsRemembered()
    {
        auto first = player(QStringLiteral("vela-remembered"));
        const quint32 id = first->waitStreaming();
        QTRY_VERIFY(stream(id));
        QVERIFY(core->setTarget(id, headphones));
        QTRY_COMPARE(linkedTo(id), headphones);
        QTest::qWait(wireplumberSaveDelayMs);
        QCOMPARE(first->close(), 0);

        // The next time the app plays, WirePlumber sends it there again.
        auto again = player(QStringLiteral("vela-remembered"));
        const quint32 next = again->waitStreaming();
        QTRY_COMPARE(linkedTo(next), headphones);

        // Back to the default one; forgotten.
        QVERIFY(core->setTarget(next, QString()));
        QTRY_COMPARE(linkedTo(next), speakers);
        QTest::qWait(wireplumberSaveDelayMs);
        QCOMPARE(again->close(), 0);
        auto third = player(QStringLiteral("vela-remembered"));
        const quint32 last = third->waitStreaming();
        QTRY_COMPARE(linkedTo(last), speakers);
    }

    void inputChosenForAStream()
    {
        QVERIFY(control(QStringLiteral("add-source vela-test-usb-mic")).startsWith(QLatin1String("ok")));
        QTRY_VERIFY(endpoint(QStringLiteral("vela-test-usb-mic")));
        auto voice = recorder(QStringLiteral("vela-caller"));
        const quint32 id = voice->waitStreaming();
        QTRY_COMPARE(linkedTo(id), microphone);
        QVERIFY(core->setTarget(id, QStringLiteral("vela-test-usb-mic")));
        QTRY_COMPARE(linkedTo(id), QStringLiteral("vela-test-usb-mic"));
        QCOMPARE(targetOf(id), QStringLiteral("vela-test-usb-mic"));
        QVERIFY(core->setTarget(id, QString()));
        QTRY_COMPARE(linkedTo(id), microphone);
        QCOMPARE(voice->close(), 0);
        QCOMPARE(control(QStringLiteral("remove vela-test-usb-mic")), QStringLiteral("ok"));
    }

    void devicesAppearAndDisappear()
    {
        QVERIFY(control(QStringLiteral("add-sink vela-test-usb")).startsWith(QLatin1String("ok")));
        QTRY_VERIFY(endpoint(QStringLiteral("vela-test-usb")));
        QCOMPARE(endpoint(QStringLiteral("vela-test-usb"))->direction, Direction::Output);
        QCOMPARE(control(QStringLiteral("remove vela-test-usb")), QStringLiteral("ok"));
        QTRY_VERIFY(!endpoint(QStringLiteral("vela-test-usb")));
    }

    void wireplumberRestart()
    {
        QCOMPARE(control(QStringLiteral("stop-wireplumber")), QStringLiteral("ok"));
        // Still connected to PipeWire; the defaults are unknown, not "none".
        QTRY_VERIFY(!core->snapshot().defaultsKnown);
        QVERIFY(core->connected());
        QVERIFY(core->snapshot().defaultOutput.isEmpty());
        QVERIFY(!core->setDefault(Direction::Output, headphones));
        QCOMPARE(control(QStringLiteral("start-wireplumber")), QStringLiteral("ok"));
        QTRY_VERIFY(core->snapshot().defaultsKnown);
        QTRY_COMPARE(core->snapshot().defaultOutput, speakers);
    }

    void pipewireRestart()
    {
        QSignalSpy connection(core.get(), &Core::connectedChanged);
        QCOMPARE(control(QStringLiteral("stop-pipewire")), QStringLiteral("ok"));
        QTRY_VERIFY(!core->connected());
        QCOMPARE(linkedTo(1), QStringLiteral("(Core is not connected)"));
        QCOMPARE(targetOf(1), QStringLiteral("(Core is not connected)"));
        QVERIFY(core->snapshot().endpoints.isEmpty());
        QVERIFY(!core->setVolume(1, 0.5));
        QCOMPARE(control(QStringLiteral("start-pipewire")), QStringLiteral("ok"));
        QTRY_VERIFY_WITH_TIMEOUT(core->connected() && core->snapshot().defaultsKnown, 10000);
        QTRY_VERIFY(endpoint(speakers));
        QCOMPARE(core->snapshot().defaultOutput, speakers);
        QVERIFY(connection.size() >= 2);
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "core.moc"
