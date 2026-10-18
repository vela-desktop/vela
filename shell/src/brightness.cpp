// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "brightness.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace {

QString sessionSocket()
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString display = qEnvironmentVariable("WAYLAND_DISPLAY", QStringLiteral("wayland-0"));
    return runtimeDir + QStringLiteral("/vela-") + display + QStringLiteral(".sock");
}

qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

Brightness::Brightness(QObject* parent, const QString& socketPath, int timeoutMs)
    : QObject(parent)
    , m_path(socketPath.isEmpty() ? sessionSocket() : socketPath)
    , m_timeout(timeoutMs)
{
    // The compositor may start after us, or restart: try again every second.
    m_retry.setSingleShot(true);
    m_retry.setInterval(1000);
    connect(&m_retry, &QTimer::timeout, this, &Brightness::connectToCompositor);
    m_expiry.setSingleShot(true);
    connect(&m_expiry, &QTimer::timeout, this, &Brightness::expire);
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        m_connected = true;
        send("watch brightness");
        if (m_live) {
            send("brightness-live on");
        }
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &Brightness::read);
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        m_connected = false;
        m_buffer.clear();
        m_outputs.clear();
        m_wanted.clear();
        emit changed();
        m_retry.start();
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] {
        if (m_socket.state() == QLocalSocket::UnconnectedState) {
            m_retry.start();
        }
    });
    connectToCompositor();
}

Brightness::~Brightness()
{
    // The socket closes after the rest of the object is gone: its
    // `disconnected` must not reach it.
    disconnect(&m_socket, nullptr, this, nullptr);
}

void Brightness::connectToCompositor()
{
    if (m_socket.state() == QLocalSocket::UnconnectedState) {
        m_socket.connectToServer(m_path);
    }
}

void Brightness::send(const QByteArray& line)
{
    if (m_socket.state() == QLocalSocket::ConnectedState) {
        m_socket.write(line + '\n');
    }
}

void Brightness::setLive(bool on)
{
    if (on == m_live) {
        return;
    }
    m_live = on;
    send(on ? "brightness-live on" : "brightness-live off");
    emit liveChanged();
}

void Brightness::read()
{
    m_buffer += m_socket.readAll();
    // Every line is the whole state: only the newest matters.
    const qsizetype end = m_buffer.lastIndexOf('\n');
    if (end < 0) {
        return;
    }
    const qsizetype start = m_buffer.lastIndexOf('\n', end - 1) + 1;
    const QByteArray line = m_buffer.mid(start, end - start);
    m_buffer.remove(0, end + 1);
    apply(line);
}

void Brightness::apply(const QByteArray& line)
{
    const QJsonObject state = QJsonDocument::fromJson(line).object();
    if (!state.value(QStringLiteral("outputs")).isArray()) {
        return;
    }
    QList<Output> outputs;
    for (const QJsonValue& value : state.value(QStringLiteral("outputs")).toArray()) {
        const QJsonObject o = value.toObject();
        Output output;
        output.name = o.value(QStringLiteral("name")).toString();
        const QString description = o.value(QStringLiteral("description")).toString();
        output.label = description.isEmpty() ? output.name : description;
        output.mechanism = o.value(QStringLiteral("mechanism")).toString();
        output.state = o.value(QStringLiteral("state")).toString();
        output.value = o.value(QStringLiteral("value")).isDouble() ? o.value(QStringLiteral("value")).toDouble() : -1;
        output.pending = o.value(QStringLiteral("pending")).toBool();
        output.refused = o.value(QStringLiteral("refused")).toBool();
        output.max = quint32(o.value(QStringLiteral("max")).toDouble());
        output.seq = quint64(o.value(QStringLiteral("seq")).toDouble());
        auto wanted = m_wanted.find(output.name);
        // Taken (or a newer request since): the compositor's value is the one.
        if (wanted != m_wanted.end() && (output.seq >= wanted->expect || !available(output))) {
            m_wanted.erase(wanted);
        }
        if (!output.name.isEmpty()) {
            outputs.append(output);
        }
    }
    for (auto it = m_wanted.begin(); it != m_wanted.end();) {
        const bool gone = std::none_of(outputs.begin(), outputs.end(), [&](const Output& o) { return o.name == it.key(); });
        it = gone ? m_wanted.erase(it) : std::next(it);
    }
    m_outputs = outputs;
    emit changed();
}

bool Brightness::available(const Output& output)
{
    return (output.state == QLatin1String("ready") || output.state == QLatin1String("not-responding"))
        && output.value >= 0 && output.max > 0;
}

QVariantList Brightness::outputs() const
{
    QVariantList list;
    for (const Output& output : m_outputs) {
        list.append(this->output(output.name));
    }
    return list;
}

QVariantMap Brightness::output(const QString& name) const
{
    for (const Output& output : m_outputs) {
        if (output.name != name) {
            continue;
        }
        const auto wanted = m_wanted.constFind(name);
        const bool asked = wanted != m_wanted.constEnd();
        return {
            { QStringLiteral("name"), output.name },
            { QStringLiteral("label"), output.label },
            { QStringLiteral("mechanism"), output.mechanism },
            { QStringLiteral("available"), available(output) },
            { QStringLiteral("locked"), output.state == QLatin1String("not-responding") },
            { QStringLiteral("pending"), asked || output.pending },
            { QStringLiteral("refused"), !asked && output.refused },
            { QStringLiteral("reason"), output.state },
            { QStringLiteral("value"), asked ? wanted->value : std::max(0.0, output.value) },
        };
    }
    return {};
}

void Brightness::setValue(const QString& name, double value)
{
    const auto output = std::find_if(m_outputs.begin(), m_outputs.end(), [&](const Output& o) { return o.name == name; });
    if (output == m_outputs.end() || !available(*output) || output->state != QLatin1String("ready")
        || !std::isfinite(value)) {
        return;
    }
    value = std::clamp(value, 0.0, 1.0);
    // The compositor rounds to the device's units, and numbers the target
    // only when that changes what it shows (brightness_model.c).
    const quint32 raw = quint32(std::lround(value * output->max));
    const auto previous = m_wanted.constFind(name);
    const bool asking = previous != m_wanted.constEnd();
    const quint32 shown = asking ? previous->raw : quint32(std::lround(output->value * output->max));
    const quint64 base = asking ? previous->expect : output->seq;
    if (asking || raw != shown) {
        m_wanted.insert(name, { value, raw, raw != shown ? base + 1 : base, now() + m_timeout });
        if (!m_expiry.isActive()) {
            m_expiry.start(m_timeout);
        }
    }
    // Sent even when it shows already: it clears a refusal's note.
    send("brightness " + name.toUtf8() + ' ' + QByteArray::number(value, 'g', 9));
    emit changed();
}

void Brightness::step(const QString& name, int direction)
{
    const QVariantMap entry = output(name);
    if (entry.value(QStringLiteral("available")).toBool()) {
        setValue(name, entry.value(QStringLiteral("value")).toDouble() + (direction > 0 ? wheelStep : -wheelStep));
    }
}

void Brightness::probe(const QString& name)
{
    send("brightness-probe " + name.toUtf8());
}

// A request the compositor never took (it wasn't ready after all): the slider
// shows the compositor's value again.
void Brightness::expire()
{
    const qint64 time = now();
    qint64 next = 0;
    bool dropped = false;
    for (auto it = m_wanted.begin(); it != m_wanted.end();) {
        if (it->deadline <= time) {
            it = m_wanted.erase(it);
            dropped = true;
        } else {
            next = next ? std::min(next, it->deadline) : it->deadline;
            ++it;
        }
    }
    if (next) {
        m_expiry.start(int(next - time));
    }
    if (dropped) {
        emit changed();
    }
}
