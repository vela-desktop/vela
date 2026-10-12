// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Vela's own streams against a real PipeWire and WirePlumber (docs/audio.md
// §7): level meters within a tolerance of a known amplitude, only while
// active and never counted as apps; the test sound reaching only the chosen
// output, left channel then right, and stopping when that output goes away.

#include "audio.h"
#include "core.h"
#include "harness.h"
#include "levelmeter.h"
#include "testsound.h"

#include <QSignalSpy>

#include <cmath>

using namespace harness;
using vela::audio::Audio;
using vela::audio::LevelMeter;
using vela::audio::TestSound;

namespace {

// A level on the meter's scale (cubic) for a linear amplitude.
double levelOf(double amplitude)
{
    return std::cbrt(amplitude);
}

bool near(double a, double b, double tolerance = 0.02)
{
    return std::abs(a - b) < tolerance;
}

// What an output plays, recorded from its monitor: each channel's peaks.
std::unique_ptr<FakeStream> listener(const QString& output, int channels = 2)
{
    return std::make_unique<FakeStream>(QStringList { QStringLiteral("--record"), QStringLiteral("--channels"),
        QString::number(channels), QStringLiteral("--prop"), QStringLiteral("stream.capture.sink=true"),
        QStringLiteral("--prop"), QStringLiteral("target.object=") + output, QStringLiteral("--prop"),
        QStringLiteral("node.dont-reconnect=true"), QStringLiteral("--prop"),
        QStringLiteral("application.process.binary=vela-listener") });
}

// A sine of that amplitude into an endpoint (a fake microphone hears it).
std::unique_ptr<FakeStream> tone(const QString& binary, double amplitude, const QString& target = {},
    const QStringList& extra = {})
{
    QStringList more = QStringList { QStringLiteral("--amplitude"), QString::number(amplitude) } + extra;
    if (!target.isEmpty()) {
        more << QStringLiteral("--prop") << QStringLiteral("target.object=") + target;
    }
    return player(binary, more);
}

} // namespace

class StreamsTest : public QObject {
    Q_OBJECT

    std::unique_ptr<Audio> audio;

    quint32 id(const QString& name) const
    {
        const auto* e = audio->core()->snapshot().endpoint(name);
        return e ? e->id : 0;
    }
    std::unique_ptr<LevelMeter> meter(const QString& kind, const QString& name)
    {
        auto m = std::make_unique<LevelMeter>();
        m->setAudio(audio.get());
        m->setKind(kind);
        m->setName(name);
        m->setActive(true);
        return m;
    }

private slots:
    void init()
    {
        audio = std::make_unique<Audio>();
        QTRY_VERIFY_WITH_TIMEOUT(audio->available() && id(speakers) && id(microphone), 5000);
    }

    void cleanup()
    {
        QTRY_VERIFY_WITH_TIMEOUT(audio->available() && id(speakers) && id(microphone), 10000);
        for (const QString& name : { speakers, headphones, surround, microphone }) {
            run(QStringLiteral("wpctl"), { QStringLiteral("set-volume"), QString::number(id(name)), QStringLiteral("1") });
        }
        audio.reset();
    }

    void outputLevelFollowsAmplitudeAndVolume()
    {
        auto music = tone(QStringLiteral("vela-player"), 0.5);
        QVERIFY(music->waitStreaming());
        auto m = meter(QStringLiteral("output"), speakers);
        QTRY_VERIFY_WITH_TIMEOUT(near(m->level(), levelOf(0.5)), 5000);
        // 50% on the cubic scale is 0.125 linear: the monitor hears 0.0625.
        run(QStringLiteral("wpctl"), { QStringLiteral("set-volume"), QString::number(id(speakers)), QStringLiteral("0.5") });
        QTRY_VERIFY_WITH_TIMEOUT(near(m->level(), levelOf(0.0625)), 5000);
    }

    void inputLevelOnlyWhileActive()
    {
        // WirePlumber never sends a playing stream to a source: the voice
        // reaches the fake microphone through a link made here (its ports
        // follow the speakers' layout).
        auto voice = tone(QStringLiteral("vela-voice"), 0.25, QString(),
            { QStringLiteral("--channels"), QStringLiteral("1"), QStringLiteral("--prop"),
                QStringLiteral("node.name=vela-voice") });
        QVERIFY(voice->waitStreaming());
        run(QStringLiteral("pw-link"), { QStringLiteral("vela-voice:output_FL"), microphone + QStringLiteral(":input_MONO") });
        auto m = meter(QStringLiteral("input"), microphone);
        QTRY_VERIFY2_WITH_TIMEOUT(near(m->level(), levelOf(0.25)), qPrintable(QString::number(m->level())), 5000);
        QCOMPARE(m->monitored(), QList<quint32>({ id(microphone) }));

        // Not shown: no stream, and the bar is empty.
        m->setActive(false);
        QVERIFY(m->monitored().isEmpty());
        QCOMPARE(m->level(), 0.0);
        QTRY_VERIFY(std::none_of(audio->core()->snapshot().streams.begin(), audio->core()->snapshot().streams.end(),
            [](const vela::audio::Stream& s) { return s.monitor; }));
    }

    void appLevelCoversAllItsStreams()
    {
        auto quiet = tone(QStringLiteral("vela-player"), 0.2);
        auto loud = tone(QStringLiteral("vela-player"), 0.4);
        const quint32 q = quiet->waitStreaming();
        const quint32 l = loud->waitStreaming();
        QVERIFY(q && l);
        auto m = meter(QStringLiteral("app"), QStringLiteral("vela-player"));
        QTRY_VERIFY_WITH_TIMEOUT(m->monitored().size() == 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(near(m->level(), levelOf(0.4)), 5000);

        // The meters aren't apps, and only one app is there.
        QTest::qWait(200);
        QCOMPARE(audio->apps().size(), 1);

        // A stream ends: its monitor goes with it.
        QCOMPARE(loud->close(), 0);
        QTRY_COMPARE(m->monitored(), QList<quint32>({ q }));
        QTRY_VERIFY_WITH_TIMEOUT(near(m->level(), levelOf(0.2)), 5000);
    }

    void meterTracksReplacementStreams()
    {
        auto first = tone(QStringLiteral("vela-player"), 0.3);
        QVERIFY(first->waitStreaming());
        auto m = meter(QStringLiteral("app"), QStringLiteral("vela-player"));
        QTRY_VERIFY_WITH_TIMEOUT(near(m->level(), levelOf(0.3)), 5000);
        QCOMPARE(m->monitored().size(), 1);
        QCOMPARE(first->close(), 0);
        QTRY_VERIFY(m->monitored().isEmpty());
        auto second = tone(QStringLiteral("vela-player"), 0.5);
        QVERIFY(second->waitStreaming());
        QTRY_VERIFY_WITH_TIMEOUT(near(m->level(), levelOf(0.5)), 5000);
        QCOMPARE(m->monitored(), QList<quint32>({ second->node() }));
        // Different streams must never retain the previous node's meter.
        QVERIFY(m->monitored().first() == second->node());
    }

    void metersSurviveARestart()
    {
        auto music = tone(QStringLiteral("vela-player"), 0.5);
        QVERIFY(music->waitStreaming());
        auto m = meter(QStringLiteral("output"), speakers);
        QTRY_VERIFY_WITH_TIMEOUT(near(m->level(), levelOf(0.5)), 5000);
        QCOMPARE(control(QStringLiteral("restart-pipewire")), QStringLiteral("ok"));
        QTRY_VERIFY_WITH_TIMEOUT(m->monitored().isEmpty() || !audio->available(), 5000);
        music.reset();
        QTRY_VERIFY_WITH_TIMEOUT(audio->available() && id(speakers), 10000);
        auto again = tone(QStringLiteral("vela-player"), 0.5);
        QVERIFY(again->waitStreaming());
        QTRY_VERIFY_WITH_TIMEOUT(near(m->level(), levelOf(0.5)), 10000);
    }

    void streamsGoBeforeTheConnection()
    {
        // PipeWire goes away while a meter records and the chime plays.
        auto music = tone(QStringLiteral("vela-player"), 0.5);
        QVERIFY(music->waitStreaming());
        auto m = meter(QStringLiteral("output"), speakers);
        QTRY_VERIFY_WITH_TIMEOUT(!m->monitored().isEmpty(), 5000);
        audio->playTestSound(speakers);
        QCOMPARE(audio->testSoundOutput(), speakers);
        QCOMPARE(control(QStringLiteral("stop-pipewire")), QStringLiteral("ok"));
        QTRY_VERIFY(!audio->available());
        QVERIFY(m->monitored().isEmpty());
        QVERIFY(audio->testSoundOutput().isEmpty());
        music.reset();
        QCOMPARE(control(QStringLiteral("start-pipewire")), QStringLiteral("ok"));
        QTRY_VERIFY_WITH_TIMEOUT(audio->available() && id(speakers), 10000);

        // The library goes away while the chime plays and the meter records:
        // nothing outlives the connection (a sanitizer would say).
        auto again = tone(QStringLiteral("vela-player"), 0.5);
        QVERIFY(again->waitStreaming());
        QTRY_VERIFY_WITH_TIMEOUT(!m->monitored().isEmpty(), 5000);
        audio->playTestSound(speakers);
        audio.reset();
        QVERIFY(m->monitored().isEmpty());
        QCOMPARE(m->level(), 0.0);
        m.reset();
        audio = std::make_unique<Audio>();
        QTRY_VERIFY_WITH_TIMEOUT(audio->available() && id(speakers), 5000);
    }

    void chimeShape()
    {
        // Stereo: the left channel first, then the right, never both.
        const QList<float> stereo = TestSound::synthesize(2, 48000);
        const qsizetype frames = stereo.size() / 2;
        float leftFirst = 0.0f;
        float rightFirst = 0.0f;
        float leftSecond = 0.0f;
        float rightSecond = 0.0f;
        for (qsizetype i = 0; i < frames; i++) {
            const bool first = i < frames / 2;
            (first ? leftFirst : leftSecond) = std::max(first ? leftFirst : leftSecond, std::abs(stereo[i * 2]));
            (first ? rightFirst : rightSecond) = std::max(first ? rightFirst : rightSecond, std::abs(stereo[i * 2 + 1]));
        }
        QVERIFY(leftFirst > 0.1f && rightSecond > 0.1f);
        QCOMPARE(rightFirst, 0.0f);
        QCOMPARE(leftSecond, 0.0f);
        QVERIFY(leftFirst < 0.5f); // soft
        // Mono: once.
        QVERIFY(TestSound::synthesize(1, 48000).size() < frames);
    }

    void testSoundLeftThenRightOnlyThere()
    {
        // The default output is the speakers; the sound goes to the
        // headphones only.
        auto chosen = listener(headphones);
        auto other = listener(speakers);
        QVERIFY(chosen->waitStreaming() && other->waitStreaming());
        QSignalSpy changed(audio.get(), &Audio::testSoundChanged);
        audio->playTestSound(headphones);
        QCOMPARE(audio->testSoundOutput(), headphones);
        QTRY_VERIFY_WITH_TIMEOUT(audio->testSoundOutput().isEmpty(), 5000);
        QVERIFY(changed.size() >= 2);

        // Each report is ~100 ms: left alone first, right alone afterwards.
        qsizetype firstLeft = -1;
        qsizetype firstRight = -1;
        for (qsizetype i = 0; i < chosen->peaks().size(); i++) {
            const QList<double>& p = chosen->peaks()[i];
            if (firstLeft < 0 && p.value(0) > 0.05) {
                firstLeft = i;
            }
            if (firstRight < 0 && p.value(1) > 0.05) {
                firstRight = i;
            }
        }
        QVERIFY2(firstLeft >= 0 && firstRight > firstLeft, "left, then right");
        QVERIFY(chosen->peaks()[firstLeft].value(1) < 0.01);
        for (const QList<double>& p : other->peaks()) {
            QVERIFY2(p.value(0) < 0.001 && p.value(1) < 0.001, "nothing on the other output");
        }
    }

    void testSoundAgainAndOnAMonoOutput()
    {
        QVERIFY(control(QStringLiteral("add-sink vela-test-mono MONO")).startsWith(QLatin1String("ok")));
        QTRY_VERIFY(id(QStringLiteral("vela-test-mono")));
        auto mono = listener(QStringLiteral("vela-test-mono"), 1);
        QVERIFY(mono->waitStreaming());
        audio->playTestSound(QStringLiteral("vela-test-mono"));
        QTest::qWait(300);
        // A second press starts it again.
        audio->playTestSound(QStringLiteral("vela-test-mono"));
        QCOMPARE(audio->testSoundOutput(), QStringLiteral("vela-test-mono"));
        QTRY_VERIFY_WITH_TIMEOUT(audio->testSoundOutput().isEmpty(), 5000);
        const auto& peaks = mono->peaks();
        QVERIFY(std::any_of(peaks.begin(), peaks.end(), [](const QList<double>& p) { return p.value(0) > 0.05; }));
        QCOMPARE(mono->close(), 0);
        QCOMPARE(control(QStringLiteral("remove vela-test-mono")), QStringLiteral("ok"));
    }

    void testSoundStopsWhenTheOutputGoes()
    {
        QVERIFY(control(QStringLiteral("add-sink vela-test-usb FL FR")).startsWith(QLatin1String("ok")));
        QTRY_VERIFY(id(QStringLiteral("vela-test-usb")));
        auto fallback = listener(speakers);
        QVERIFY(fallback->waitStreaming());
        audio->playTestSound(QStringLiteral("vela-test-usb"));
        QTest::qWait(100);
        QCOMPARE(control(QStringLiteral("remove vela-test-usb")), QStringLiteral("ok"));
        QTRY_VERIFY_WITH_TIMEOUT(audio->testSoundOutput().isEmpty(), 2000);
        // It didn't move to the default output.
        QTest::qWait(300);
        for (const QList<double>& p : fallback->peaks()) {
            QVERIFY(p.value(0) < 0.001 && p.value(1) < 0.001);
        }
    }
};

QTEST_GUILESS_MAIN(StreamsTest)
#include "streams.moc"
