// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The shell's sound keys (shell/src/soundkeys.cpp, docs/audio.md §6.1) on
// vela-audio, against the fixture: volume keys and their indicator, the
// microphone mute key whose indicator waits for the server, and the wheel on
// an app's taskbar button.

#include "audio.h"
#include "core.h"
#include "harness.h"
#include "soundkeys.h"

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

} // namespace

class KeysTest : public QObject {
    Q_OBJECT

    std::unique_ptr<Audio> audio;
    std::unique_ptr<SoundKeys> keys;

    double serverVolume(const QString& name) const
    {
        const auto* e = audio->core()->snapshot().endpoint(name);
        return e ? e->volume() : -1.0;
    }
    bool serverMuted(const QString& name) const
    {
        const auto* e = audio->core()->snapshot().endpoint(name);
        return e && e->muted;
    }

private slots:
    void init()
    {
        audio = std::make_unique<Audio>();
        keys = std::make_unique<SoundKeys>(audio.get());
        QTRY_VERIFY_WITH_TIMEOUT(audio->available() && audio->defaultOutput() == speakers
                && audio->defaultInput() == microphone,
            5000);
    }

    void cleanup()
    {
        for (const QString& name : { speakers, microphone }) {
            const QString id = QString::number(audio->core()->snapshot().endpoint(name)->id);
            run(QStringLiteral("wpctl"), { QStringLiteral("set-volume"), id, QStringLiteral("1") });
            run(QStringLiteral("wpctl"), { QStringLiteral("set-mute"), id, QStringLiteral("0") });
        }
        keys.reset();
        audio.reset();
    }

    void volumeKeys()
    {
        QSignalSpy osd(keys.get(), &SoundKeys::osdRequested);
        keys->press(QStringLiteral("down"));
        QCOMPARE(osd.size(), 1);
        QCOMPARE(osd.first().first().toString(), QString());
        QTRY_VERIFY(near(serverVolume(speakers), 0.98));
        keys->press(QStringLiteral("down"));
        QTRY_VERIFY(near(serverVolume(speakers), 0.96));
        keys->press(QStringLiteral("up"));
        QTRY_VERIFY(near(serverVolume(speakers), 0.98));
        QCOMPARE(osd.size(), 3);

        keys->press(QStringLiteral("mute"));
        QVERIFY(audio->muted());
        QTRY_VERIFY(serverMuted(speakers));
        // Up unmutes, like Windows.
        keys->press(QStringLiteral("up"));
        QTRY_VERIFY(!serverMuted(speakers));
        keys->press(QStringLiteral("nonsense"));
        QCOMPARE(osd.size(), 5);
    }

    void microphoneKeyWaitsForTheServer()
    {
        QSignalSpy osd(keys.get(), &SoundKeys::osdRequested);
        keys->press(QStringLiteral("mic-mute"));
        // Nothing yet: the indicator would show a state that isn't true.
        QCOMPARE(osd.size(), 0);
        QVERIFY(!audio->inputMuted());
        QTRY_COMPARE(osd.size(), 1);
        QCOMPARE(osd.first().first().toString(), QStringLiteral("microphone"));
        QVERIFY(audio->inputMuted());
        QVERIFY(serverMuted(microphone));

        keys->press(QStringLiteral("mic-mute"));
        QTRY_COMPARE(osd.size(), 2);
        QVERIFY(!audio->inputMuted());
        QVERIFY(!serverMuted(microphone));
    }

    void repeatedMicrophoneKeysHonorFinalIntent()
    {
        QSignalSpy osd(keys.get(), &SoundKeys::osdRequested);
        keys->press(QStringLiteral("mic-mute"));
        keys->press(QStringLiteral("mic-mute")); // before the first confirmation
        QCOMPARE(osd.size(), 0);
        QTRY_COMPARE(osd.size(), 1);
        QVERIFY(!audio->inputMuted());
        QVERIFY(!serverMuted(microphone));

        keys->press(QStringLiteral("mic-mute"));
        keys->press(QStringLiteral("mic-mute"));
        keys->press(QStringLiteral("mic-mute"));
        QTRY_COMPARE(osd.size(), 2);
        QVERIFY(audio->inputMuted());
        QVERIFY(serverMuted(microphone));
    }

    void taskbarWheel()
    {
        audio->setAppResolver([](const AppIdentity& id) {
            return id.binary == QLatin1String("vela-player")
                ? AppInfo { QStringLiteral("org.vela.Player.desktop"), QStringLiteral("Vela Player"), QString() }
                : AppInfo {};
        });
        QSignalSpy osd(keys.get(), &SoundKeys::osdRequested);
        QVERIFY(!keys->stepTaskVolume(QStringLiteral("org.vela.Player.desktop"), 1)); // not playing
        auto music = player(QStringLiteral("vela-player"));
        const quint32 stream = music->waitStreaming();
        QTRY_VERIFY(!audio->appKey(QStringLiteral("org.vela.Player.desktop")).isEmpty());
        QVERIFY(keys->stepTaskVolume(QStringLiteral("org.vela.Player.desktop"), -1));
        QCOMPARE(osd.size(), 1);
        QCOMPARE(osd.first().first().toString(), QStringLiteral("vela-player"));
        QTRY_VERIFY(near(audio->core()->snapshot().stream(stream)->volume(), 0.98));
        QVERIFY(!keys->stepTaskVolume(QStringLiteral("org.other.App.desktop"), -1));
        QCOMPARE(music->close(), 0);

        // The same desktop application may record without playing anything.
        // The wheel must not report a volume change for a recording stream.
        auto voice = recorder(QStringLiteral("vela-player"));
        QVERIFY(voice->waitStreaming());
        QTRY_VERIFY([&] {
            for (const QVariant& entry : audio->apps()) {
                const QVariantMap app = entry.toMap();
                if (app.value(QStringLiteral("key")).toString() == QLatin1String("vela-player")) {
                    return app.value(QStringLiteral("recording")).toBool()
                        && !app.value(QStringLiteral("playing")).toBool();
                }
            }
            return false;
        }());
        QVERIFY(!keys->stepTaskVolume(QStringLiteral("org.vela.Player.desktop"), -1));
        QCOMPARE(osd.size(), 1);
        QCOMPARE(voice->close(), 0);
    }
};

QTEST_GUILESS_MAIN(KeysTest)
#include "keys.moc"
