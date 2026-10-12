// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "testsound.h"

#include "core.h"

#include <QCoreApplication>

#include <atomic>
#include <cmath>
#include <numbers>

namespace vela::audio {

namespace {

constexpr int rate = 48000;
// Silence before the chime. An output waking up (a USB DAC unmuting, a
// device switching rate) loses its first moments: the chime's start
// would be cut.
constexpr double leadIn = 0.25; // s

// One chime: two soft tones, the second a little after the first.
void chime(QList<float>& out, int channels, int channel, qsizetype start, int sampleRate)
{
    struct Tone {
        double frequency;
        double delay; // s
    };
    constexpr Tone tones[] = { { 880.0, 0.0 }, { 659.25, 0.13 } };
    constexpr double length = 0.6; // s, until the second tone has faded
    const qsizetype frames = qsizetype(length * sampleRate);
    for (const Tone& tone : tones) {
        for (qsizetype i = 0; i < frames; i++) {
            const double t = double(i) / sampleRate - tone.delay;
            if (t < 0.0) {
                continue;
            }
            const double attack = std::min(1.0, t / 0.008);
            const double envelope = attack * std::exp(-t / 0.11);
            const double phase = 2.0 * std::numbers::pi * tone.frequency * t;
            const float value = float(0.22 * envelope * (std::sin(phase) + 0.15 * std::sin(2.0 * phase)));
            for (int c = 0; c < channels; c++) {
                if (channel < 0 || c == channel) {
                    out[(start + i) * channels + c] += value;
                }
            }
        }
    }
}

} // namespace

QList<float> TestSound::synthesize(int channels, int sampleRate)
{
    const qsizetype side = qsizetype(0.6 * sampleRate);
    const qsizetype gap = qsizetype(0.15 * sampleRate);
    const bool stereo = channels == 2;
    QList<float> out((stereo ? 2 * side + gap : side) * channels, 0.0f);
    if (stereo) {
        chime(out, channels, 0, 0, sampleRate);
        chime(out, channels, 1, side + gap, sampleRate);
    } else {
        chime(out, channels, -1, 0, sampleRate);
    }
    return out;
}

struct TestSound::Playback {
    TestSound* owner = nullptr;
    quint64 generation = 0; // a late "drained" can't stop the next one
    pw_stream* stream = nullptr;
    spa_hook listener {};
    int channels = 2;
    QList<float> samples;
    // Read by PipeWire's real-time thread only, once the stream runs.
    qsizetype position = 0;
    bool draining = false;

    ~Playback()
    {
        if (stream) {
            spa_hook_remove(&listener);
            pw_stream_destroy(stream);
        }
    }

    static void onProcess(void* data)
    {
        auto* p = static_cast<Playback*>(data);
        pw_buffer* b = pw_stream_dequeue_buffer(p->stream);
        if (!b) {
            return;
        }
        spa_data& d = b->buffer->datas[0];
        if (d.data) {
            const uint32_t stride = sizeof(float) * uint32_t(p->channels);
            uint32_t frames = d.maxsize / stride;
            if (b->requested > 0) {
                frames = std::min<uint32_t>(frames, uint32_t(b->requested));
            }
            auto* out = static_cast<float*>(d.data);
            const qsizetype left = (p->samples.size() - p->position) / p->channels;
            const qsizetype n = std::min<qsizetype>(frames, std::max<qsizetype>(left, 0));
            std::copy_n(p->samples.constData() + p->position, n * p->channels, out);
            std::fill(out + n * p->channels, out + qsizetype(frames) * p->channels, 0.0f);
            p->position += n * p->channels;
            d.chunk->offset = 0;
            d.chunk->stride = int32_t(stride);
            d.chunk->size = frames * stride;
        }
        pw_stream_queue_buffer(p->stream, b);
        if (p->position >= p->samples.size() && !p->draining) {
            p->draining = true;
            pw_stream_flush(p->stream, true);
        }
    }

    // Both on the interface's thread (PipeWire's main loop is Qt's).
    static void onDrained(void* data)
    {
        auto* p = static_cast<Playback*>(data);
        QMetaObject::invokeMethod(
            p->owner, [owner = p->owner, generation = p->generation] { owner->finished(generation); },
            Qt::QueuedConnection);
    }
    static void onStateChanged(void* data, pw_stream_state, pw_stream_state state, const char*)
    {
        if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
            onDrained(data); // the output went away: it stops
        }
    }

    static const pw_stream_events* events()
    {
        static const pw_stream_events e = [] {
            pw_stream_events e {};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.state_changed = onStateChanged;
            e.process = onProcess;
            e.drained = onDrained;
            return e;
        }();
        return &e;
    }
};

TestSound::TestSound(Core* core, QObject* parent)
    : QObject(parent)
    , m_core(core)
{
    connect(m_core, &Core::aboutToDisconnect, this, &TestSound::stop);
}

TestSound::~TestSound() = default;

bool TestSound::play(const QString& output)
{
    stop();
    pw_core* connection = m_core->pipewire();
    const Endpoint* e = m_core->snapshot().endpoint(output);
    if (!connection || !e || e->direction != Direction::Output) {
        return false;
    }
    auto playback = std::make_shared<Playback>();
    playback->owner = this;
    playback->generation = ++m_generation;
    playback->channels = hasStereo(e->positions) ? 2 : 1;
    playback->samples = QList<float>(qsizetype(leadIn * rate) * playback->channels, 0.0f)
        + synthesize(playback->channels, rate);

    const QByteArray serial = QByteArray::number(e->serial);
    const QByteArray app = QCoreApplication::applicationName().toUtf8();
    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_NAME, "Test sound", PW_KEY_TARGET_OBJECT, serial.constData(), PW_KEY_NODE_DONT_RECONNECT,
        "true", "node.dont-fallback", "true", "node.dont-move", "true", "state.restore-props", "false",
        "state.restore-target", "false",
        // Left stays left: nothing made up for the center or the rear.
        "channelmix.upmix", "false", nullptr);
    if (!app.isEmpty()) {
        pw_properties_set(props, PW_KEY_APP_ID, app.constData());
    }
    playback->stream = pw_stream_new(connection, "vela-test-sound", props);
    if (!playback->stream) {
        return false;
    }
    pw_stream_add_listener(playback->stream, &playback->listener, Playback::events(), playback.get());

    uint8_t buffer[512];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = rate;
    info.channels = uint32_t(playback->channels);
    if (playback->channels == 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    }
    const spa_pod* params[] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info) };
    if (pw_stream_connect(playback->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
            params, 1)
        < 0) {
        return false;
    }
    m_playback = playback;
    m_output = output;
    emit playingChanged();
    return true;
}

void TestSound::stop()
{
    m_playback.reset();
    if (!m_output.isEmpty()) {
        m_output.clear();
        emit playingChanged();
    }
}

void TestSound::finished(quint64 generation)
{
    if (m_playback && m_playback->generation == generation) {
        stop();
    }
}

} // namespace vela::audio
