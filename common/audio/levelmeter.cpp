// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "levelmeter.h"

#include "audio.h"
#include "core.h"

#include <QCoreApplication>

#include <atomic>
#include <cmath>

namespace vela::audio {

namespace {

// The level shown falls by this much per tick (~30 ms): from the top to
// nothing in under a second.
constexpr double decay = 0.04;
constexpr int tickMs = 30;
// The rate peaks are computed at (resample.peaks): one per 10 ms.
constexpr int peakRate = 100;
constexpr int retryMs = 3000; // retry failed links at most once per three seconds

} // namespace

struct LevelMeter::Monitor {
    LevelMeter* meter = nullptr;
    quint32 node = 0;
    quint64 serial = 0;
    pw_stream* stream = nullptr;
    spa_hook listener {};
    // The loudest sample since the meter last looked, written by PipeWire's
    // real-time thread.
    std::atomic<float> peak { 0.0f };
    bool failed = false;

    ~Monitor()
    {
        if (stream) {
            spa_hook_remove(&listener);
            pw_stream_destroy(stream);
        }
    }

    static void onProcess(void* data)
    {
        auto* m = static_cast<Monitor*>(data);
        pw_buffer* b = pw_stream_dequeue_buffer(m->stream);
        if (!b) {
            return;
        }
        const spa_data& d = b->buffer->datas[0];
        if (d.data && d.chunk) {
            const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
            const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
            const float* samples = SPA_PTROFF(d.data, offset, const float);
            float loudest = 0.0f;
            for (uint32_t i = 0; i < size / sizeof(float); i++) {
                loudest = std::max(loudest, std::fabs(samples[i]));
            }
            if (loudest > m->peak.load(std::memory_order_relaxed)) {
                m->peak.store(loudest, std::memory_order_relaxed);
            }
        }
        pw_stream_queue_buffer(m->stream, b);
    }

    static void onStateChanged(void* data, pw_stream_state, pw_stream_state state, const char*)
    {
        auto* m = static_cast<Monitor*>(data);
        if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
            // The node went away, or WirePlumber refused the link: not from
            // inside the stream's own callback.
            m->failed = true;
            QMetaObject::invokeMethod(m->meter, &LevelMeter::reconcile, Qt::QueuedConnection);
        }
    }

    static const pw_stream_events* events()
    {
        static const pw_stream_events e = [] {
            pw_stream_events e {};
            e.version = PW_VERSION_STREAM_EVENTS;
            e.state_changed = onStateChanged;
            e.process = onProcess;
            return e;
        }();
        return &e;
    }
};

LevelMeter::LevelMeter(QObject* parent)
    : QObject(parent)
{
    m_retry.setSingleShot(true);
    m_retry.setInterval(retryMs);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        m_failed.clear();
        reconcile();
    });
    m_tick.setInterval(tickMs);
    connect(&m_tick, &QTimer::timeout, this, &LevelMeter::tick);
    m_reconcile.setSingleShot(true);
    m_reconcile.setInterval(0);
    connect(&m_reconcile, &QTimer::timeout, this, &LevelMeter::reconcile);
}

LevelMeter::~LevelMeter()
{
    closeAll();
}

void LevelMeter::setAudio(Audio* audio)
{
    if (m_audio == audio) {
        return;
    }
    if (m_audio) {
        disconnect(m_audio->core(), nullptr, this, nullptr);
    }
    closeAll();
    m_audio = audio;
    if (m_audio) {
        // Before the connection goes, the streams made on it go.
        connect(m_audio->core(), &Core::aboutToDisconnect, this, &LevelMeter::closeAll);
        connect(m_audio->core(), &Core::changed, &m_reconcile, qOverload<>(&QTimer::start));
    }
    emit audioChanged();
    reconcile();
}

void LevelMeter::setKind(const QString& kind)
{
    if (m_kind != kind) {
        m_kind = kind;
        closeAll();
        emit kindChanged();
        reconcile();
    }
}

void LevelMeter::setName(const QString& name)
{
    if (m_name != name) {
        m_name = name;
        closeAll();
        emit nameChanged();
        reconcile();
    }
}

void LevelMeter::setActive(bool active)
{
    if (m_active != active) {
        m_active = active;
        emit activeChanged();
        reconcile();
    }
}

void LevelMeter::closeAll()
{
    m_retry.stop();
    m_tick.stop();
    m_monitors.clear();
    m_failed.clear();
    if (m_level != 0.0) {
        m_level = 0.0;
        emit levelChanged();
    }
}

void LevelMeter::reconcile()
{
    Core* core = m_audio ? m_audio->core() : nullptr;
    pw_core* connection = core ? core->pipewire() : nullptr;
    // What should be recorded: node id -> {serial, capture an output}.
    struct Target {
        quint64 serial = 0;
        bool sink = false;
    };
    QHash<quint32, Target> wanted;
    if (m_active && connection && !m_name.isEmpty()) {
        const Snapshot& s = core->snapshot();
        if (m_kind == QLatin1String("app")) {
            for (const Stream& st : s.streams) {
                if (st.isApp() && st.direction == Direction::Output && st.app.key == m_name) {
                    wanted.insert(st.id, { st.serial, false });
                }
            }
        } else if (const Endpoint* e = s.endpoint(m_name)) {
            const bool output = m_kind == QLatin1String("output");
            if (e->direction == (output ? Direction::Output : Direction::Input)) {
                wanted.insert(e->id, { e->serial, output });
            }
        }
    }

    for (auto it = m_monitors.begin(); it != m_monitors.end();) {
        const bool stale = !wanted.contains(it.key()) || wanted.value(it.key()).serial != it.value()->serial;
        if (stale || it.value()->failed) {
            if (it.value()->failed) {
                m_failed.insert(it.key(), it.value()->serial);
            }
            it = m_monitors.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_failed.begin(); it != m_failed.end();) {
        it = !wanted.contains(it.key()) || wanted.value(it.key()).serial != it.value()
            ? m_failed.erase(it) : std::next(it);
    }

    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
        if (m_monitors.contains(it.key()) || m_failed.contains(it.key())) {
            continue;
        }
        const QByteArray serial = QByteArray::number(it->serial);
        const QByteArray app = QCoreApplication::applicationName().toUtf8();
        pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Monitor",
            PW_KEY_MEDIA_NAME, "Level meter", PW_KEY_STREAM_MONITOR, "true", "resample.peaks", "true",
            PW_KEY_TARGET_OBJECT, serial.constData(), PW_KEY_NODE_DONT_RECONNECT, "true", "node.dont-fallback",
            "true", "node.dont-move", "true", "state.restore-props", "false", "state.restore-target", "false",
            nullptr);
        if (it->sink) {
            pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        }
        // An input's meter opens the microphone (docs/audio.md §7.2); one
        // on an output or an app only listens while they play.
        pw_properties_set(props, PW_KEY_NODE_PASSIVE, m_kind == QLatin1String("input") ? "false" : "true");
        // Vela's own (docs/audio.md §5.2): vela-settings, vela-shell.
        if (!app.isEmpty()) {
            pw_properties_set(props, PW_KEY_APP_ID, app.constData());
        }

        auto monitor = std::make_shared<Monitor>();
        monitor->meter = this;
        monitor->node = it.key();
        monitor->serial = it->serial;
        monitor->stream = pw_stream_new(connection, "vela-level-meter", props);
        if (!monitor->stream) {
            m_failed.insert(it.key(), it->serial);
            continue;
        }
        pw_stream_add_listener(monitor->stream, &monitor->listener, Monitor::events(), monitor.get());
        uint8_t buffer[512];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_audio_info_raw info {};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = peakRate;
        const spa_pod* params[] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info) };
        if (pw_stream_connect(monitor->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                params, 1)
            < 0) {
            m_failed.insert(it.key(), it->serial);
            continue;
        }
        m_monitors.insert(it.key(), monitor);
    }

    if (m_failed.isEmpty() || !m_active || !connection) {
        m_retry.stop();
    } else if (!m_retry.isActive()) {
        m_retry.start();
    }

    if (m_monitors.isEmpty()) {
        m_tick.stop();
        if (m_level != 0.0) {
            m_level = 0.0;
            emit levelChanged();
        }
    } else if (!m_tick.isActive()) {
        m_tick.start();
    }
}

void LevelMeter::tick()
{
    float peak = 0.0f;
    for (const auto& m : std::as_const(m_monitors)) {
        peak = std::max(peak, m->peak.exchange(0.0f, std::memory_order_relaxed));
    }
    const double level = std::max(std::cbrt(std::min(double(peak), 1.0)), m_level - decay);
    if (std::abs(level - m_level) > 1e-4 || (level == 0.0 && m_level != 0.0)) {
        m_level = std::max(level, 0.0);
        emit levelChanged();
    }
}

} // namespace vela::audio
