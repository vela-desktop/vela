// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio.h"
#include "asyncprocess.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QVariantMap>

#include <utility>

namespace {

QString sinkOrSource(const QString& kind)
{
    return kind == QLatin1String("input") ? QStringLiteral("source") : QStringLiteral("sink");
}

} // namespace

Audio::Audio(QObject* parent)
    : QObject(parent)
    , m_available(!QStandardPaths::findExecutable(QStringLiteral("pactl")).isEmpty())
{
    if (!m_available) {
        return;
    }
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_debounce, &QTimer::timeout, this, &Audio::refresh);
}

QVariantList Audio::list(const QJsonArray& devices, bool inputs, const QString& defaultName) const
{
    QVariantList out;
    for (const QJsonValue& value : devices) {
        const QJsonObject device = value.toObject();
        const QString name = device[QStringLiteral("name")].toString();
        // Output "monitors" aren't microphones.
        if (inputs && name.endsWith(QLatin1String(".monitor"))) {
            continue;
        }
        // The volume: the channels' average (65536 = 100%).
        const QJsonObject channels = device[QStringLiteral("volume")].toObject();
        double sum = 0.0;
        for (const QJsonValue& channel : channels) {
            sum += channel.toObject()[QStringLiteral("value")].toDouble();
        }
        const double volume = channels.isEmpty() ? 0.0 : sum / channels.size() / 65536.0;
        out.append(QVariantMap {
            { QStringLiteral("name"), name },
            { QStringLiteral("description"), device[QStringLiteral("description")].toString() },
            { QStringLiteral("volume"), volume },
            { QStringLiteral("muted"), device[QStringLiteral("mute")].toBool() },
            { QStringLiteral("isDefault"), name == defaultName },
        });
    }
    return out;
}

void Audio::refresh()
{
    if (!m_available) {
        return;
    }
    // The first time the page opens: from then on it updates itself.
    if (!m_subscribe) {
        QProcess* subscription = vela::runProcess(this, QStringLiteral("pactl"), { QStringLiteral("subscribe") }, 0,
            [this](vela::ProcessResult) {
                m_subscribe = nullptr;
                // A later refresh can re-establish the subscription.
            });
        m_subscribe = subscription;
        connect(subscription, &QProcess::readyReadStandardOutput, this, [this, subscription] {
            const QByteArray events = subscription->readAllStandardOutput();
            // Stream events (sink-input/source-output) do not change the
            // device list; treating them as device changes can starve snapshots.
            if (events.contains("on sink #") || events.contains("on source #") || events.contains("on server")) {
                ++m_revision;
                m_debounce.start();
            }
        });
    }
    if (m_loading || m_running || !m_queue.isEmpty()) {
        m_refreshPending = true;
        return;
    }
    m_loading = true;
    m_refreshPending = false;
    const quint64 revision = m_revision;
    const QString pactl = QStringLiteral("pactl");
    vela::queryProcesses(this,
        { { QStringLiteral("output"), pactl, { QStringLiteral("get-default-sink") }, 2000 },
            { QStringLiteral("input"), pactl, { QStringLiteral("get-default-source") }, 2000 },
            { QStringLiteral("sinks"), pactl, { QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("list"), QStringLiteral("sinks") }, 2000 },
            { QStringLiteral("sources"), pactl, { QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("list"), QStringLiteral("sources") }, 2000 } },
        [this, revision](bool ok, const QMap<QString, QByteArray>& results) {
            m_loading = false;
            const QJsonDocument sinks = QJsonDocument::fromJson(results.value(QStringLiteral("sinks")));
            const QJsonDocument sources = QJsonDocument::fromJson(results.value(QStringLiteral("sources")));
            if (ok && sinks.isArray() && sources.isArray() && revision == m_revision && !m_running && m_queue.isEmpty()) {
                m_outputs = list(sinks.array(), false, QString::fromUtf8(results.value(QStringLiteral("output"))).trimmed());
                m_inputs = list(sources.array(), true, QString::fromUtf8(results.value(QStringLiteral("input"))).trimmed());
                // Only authoritative service snapshots confirm microphone state.
                m_unconfirmedMutes.clear();
                m_unconfirmedDefaults.clear();
                emit changed();
            }
            if (std::exchange(m_refreshPending, false) || revision != m_revision) {
                refresh();
            }
        });
}

void Audio::setDefault(const QString& kind, const QString& name)
{
    // Keep the last confirmed default selected until the service can be read.
    // If the command succeeds but reconciliation fails, the actual default is
    // unknown, so microphone mute must not target the cached device.
    markDefault(kind, true, false);
    run(kind + QStringLiteral(" default"), { QStringLiteral("set-default-") + sinkOrSource(kind), name });
}

void Audio::setVolume(const QString& kind, const QString& name, double volume)
{
    if (kind == QLatin1String("input") && m_unconfirmedDefaults.contains(kind)) {
        return; // the selected microphone may no longer be the real default
    }
    const int percent = qRound(qBound(0.0, volume, 1.5) * 100.0);
    run(kind + u' ' + name + QStringLiteral(" volume"),
        { QStringLiteral("set-") + sinkOrSource(kind) + QStringLiteral("-volume"), name, QString::number(percent) + u'%' });
    update(kind, name, QStringLiteral("volume"), double(percent) / 100);
}

void Audio::setMuted(const QString& kind, const QString& name, bool muted)
{
    if (kind == QLatin1String("input") && m_unconfirmedDefaults.contains(kind)) {
        return; // the selected microphone may no longer be the real default
    }
    run(kind + u' ' + name + QStringLiteral(" mute"),
        { QStringLiteral("set-") + sinkOrSource(kind) + QStringLiteral("-mute"), name,
            muted ? QStringLiteral("1") : QStringLiteral("0") });
    // Do not optimistically publish a privacy-affecting state.
    markMute(kind, name, true, false);
}

void Audio::markMute(const QString& kind, const QString& name, bool pending, bool error)
{
    QVariantList& devices = kind == QLatin1String("input") ? m_inputs : m_outputs;
    const QString key = kind + u' ' + name;
    if (pending || error) {
        m_unconfirmedMutes.insert(key);
    } else {
        m_unconfirmedMutes.remove(key);
    }
    for (QVariant& device : devices) {
        QVariantMap map = device.toMap();
        if (map.value(QStringLiteral("name")).toString() != name) {
            continue;
        }
        map.insert(QStringLiteral("mutePending"), pending);
        map.insert(QStringLiteral("muteError"), error);
        device = map;
    }
    emit changed();
}

void Audio::markDefault(const QString& kind, bool pending, bool error)
{
    if (pending || error) {
        m_unconfirmedDefaults.insert(kind);
    } else {
        m_unconfirmedDefaults.remove(kind);
    }
    QVariantList& devices = kind == QLatin1String("input") ? m_inputs : m_outputs;
    for (QVariant& device : devices) {
        QVariantMap map = device.toMap();
        map.insert(QStringLiteral("defaultPending"), pending);
        map.insert(QStringLiteral("defaultError"), error);
        device = map;
    }
    emit changed();
}

void Audio::update(const QString& kind, const QString& name, const QString& property, const QVariant& value)
{
    QVariantList& devices = kind == QLatin1String("input") ? m_inputs : m_outputs;
    for (QVariant& device : devices) {
        QVariantMap map = device.toMap();
        const bool matches = map.value(QStringLiteral("name")).toString() == name;
        if (property == QLatin1String("isDefault")) {
            map.insert(property, matches);
        } else if (matches) {
            map.insert(property, value);
        }
        device = map;
    }
    emit changed();
}

void Audio::run(const QString& key, const QStringList& arguments)
{
    ++m_revision;
    for (auto& waiting : m_queue) {
        if (waiting.first == key) {
            waiting.second = arguments;
            return;
        }
    }
    m_queue.append({ key, arguments });
    if (!m_running) {
        runNext();
    }
}

void Audio::runNext()
{
    if (m_queue.isEmpty()) {
        refresh();
        return;
    }
    m_running = true;
    const auto command = m_queue.takeFirst();
    vela::runProcess(this, QStringLiteral("pactl"), command.second, 2000, [this, command](vela::ProcessResult result) {
        if (command.first.endsWith(QStringLiteral(" default"))) {
            const QString kind = command.first.section(u' ', 0, 0);
            // Even a successful write remains unconfirmed until a valid read.
            markDefault(kind, result.ok, !result.ok);
        }
        if (command.first.endsWith(QStringLiteral(" mute"))) {
            const QString kind = command.first.section(u' ', 0, 0);
            const QString name = command.first.mid(kind.size() + 1).chopped(5);
            // A successful command still needs a fresh snapshot for confirmation.
            markMute(kind, name, result.ok, !result.ok);
        }
        m_running = false;
        runNext();
    });
}
