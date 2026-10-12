// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio.h"

#include "core.h"
#include "testsound.h"
#include "volumesteps.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>

#include <algorithm>
#include <cmath>
#include <utility>

namespace vela::audio {

namespace {

// How long a value just set is shown without the server reporting it, and
// how long a privacy-related action may stay unconfirmed.
constexpr int optimisticMs = 500;
constexpr int confirmMs = 2000;
// A profile takes longer: a Bluetooth headset switching codec or to call
// mode reconnects its audio.
constexpr int profileMs = 3000;

QString kindOf(Direction direction)
{
    return direction == Direction::Output ? QStringLiteral("output") : QStringLiteral("input");
}

bool same(const QVariant& a, const QVariant& b)
{
    if (a.typeId() == QMetaType::Double || b.typeId() == QMetaType::Double) {
        return a.isValid() && b.isValid() && std::abs(a.toDouble() - b.toDouble()) < 0.006;
    }
    return a == b;
}

QVariantMap findByKey(const QVariantList& list, const QString& field, const QString& value)
{
    for (const QVariant& item : list) {
        const QVariantMap map = item.toMap();
        if (map.value(field).toString() == value) {
            return map;
        }
    }
    return {};
}

// The position of each of an endpoint's channel volumes.
QStringList volumePositions(const Endpoint& e)
{
    if (e.volumePositions.size() == e.channelVolumes.size()) {
        return e.volumePositions;
    }
    return e.positions.size() == e.channelVolumes.size() ? e.positions : QStringList();
}

// An endpoint's balance; from silence, the one it had.
double balanceOf(const Endpoint& e)
{
    return stereoBalance(volumePositions(e), e.volume() > 0.0 ? e.channelVolumes : e.shape);
}

} // namespace

Audio::Audio(QObject* parent)
    : QObject(parent)
    , m_core(new Core(this))
    , m_testSound(new TestSound(m_core, this))
{
    connect(m_testSound, &TestSound::playingChanged, this, &Audio::testSoundChanged);
    m_flush.setSingleShot(true);
    m_flush.setInterval(0);
    connect(&m_flush, &QTimer::timeout, this, &Audio::flush);
    m_expiry.setInterval(100);
    connect(&m_expiry, &QTimer::timeout, this, &Audio::update);
    connect(m_core, &Core::changed, this, &Audio::update);
    connect(m_core, &Core::connectedChanged, this, &Audio::availableChanged);
}

Audio::~Audio() = default;

void Audio::setAppResolver(AppResolver resolver)
{
    m_resolver = std::move(resolver);
    update();
}

bool Audio::available() const
{
    return m_core->connected();
}

const Endpoint* Audio::endpoint(const QString& kind, const QString& name) const
{
    const Endpoint* e = m_core->snapshot().endpoint(name);
    return e && kindOf(e->direction) == kind ? e : nullptr;
}

QList<const Stream*> Audio::appStreams(const QString& key, Direction direction) const
{
    QList<const Stream*> out;
    for (const Stream& s : m_core->snapshot().streams) {
        if (s.isApp() && s.direction == direction && s.app.key == key) {
            out.append(&s);
        }
    }
    return out;
}

void Audio::expect(const QString& key, const QVariant& value, bool optimistic, int timeoutMs)
{
    m_errors.remove(key);
    Pending pending { value, optimistic, {}, timeoutMs > 0 ? timeoutMs : optimistic ? optimisticMs : confirmMs };
    pending.since.start();
    m_pending.insert(key, pending);
    m_expiry.start();
    update();
}

void Audio::fail(const QString& key)
{
    m_pending.remove(key);
    m_errors.insert(key);
    update();
}

// ----------------------------------------------------------------- actions --

void Audio::setDefault(const QString& kind, const QString& name)
{
    const Endpoint* e = endpoint(kind, name);
    const QString key = QStringLiteral("default ") + kind;
    if (!e || !m_core->setDefault(e->direction, name)) {
        fail(key);
        return;
    }
    expect(key, name, kind == QLatin1String("output"));
}

void Audio::setVolume(const QString& kind, const QString& name, double volume)
{
    const Endpoint* e = endpoint(kind, name);
    // The selected microphone may not be the real default yet.
    const bool inputUnsure = kind == QLatin1String("input")
        && (m_pending.contains(QStringLiteral("default input")));
    if (!e || inputUnsure) {
        return;
    }
    const QString key = QStringLiteral("volume ") + kind + u' ' + name;
    const double value = VolumeSteps::snap(volume, VolumeSteps::intervals(e->step));
    m_writes.insert(key, value);
    m_flush.start();
    expect(key, value, true);
}

void Audio::stepVolume(const QString& kind, const QString& name, int direction)
{
    const Endpoint* e = endpoint(kind, name);
    if (!e || direction == 0) {
        return;
    }
    const QVariantMap shown = findByKey(kind == QLatin1String("output") ? m_outputs : m_inputs, QStringLiteral("name"), name);
    const double current = shown.value(QStringLiteral("volume"), std::min(e->volume(), 1.0)).toDouble();
    setVolume(kind, name, VolumeSteps::step(current, direction, VolumeSteps::intervals(e->step)));
    if (kind == QLatin1String("output") && shown.value(QStringLiteral("muted")).toBool()) {
        setMuted(kind, name, false);
    }
}

void Audio::setMuted(const QString& kind, const QString& name, bool muted)
{
    const Endpoint* e = endpoint(kind, name);
    const bool input = kind == QLatin1String("input");
    if (!e || (input && (m_pending.contains(QStringLiteral("default input"))))) {
        return;
    }
    const QString key = QStringLiteral("mute ") + kind + u' ' + name;
    if (!m_core->setMuted(e->id, muted)) {
        fail(key);
        return;
    }
    expect(key, muted, !input);
}

void Audio::setBalance(const QString& kind, const QString& name, double balance)
{
    const Endpoint* e = endpoint(kind, name);
    if (!e || !hasStereo(volumePositions(*e))) {
        return;
    }
    const QString key = QStringLiteral("balance ") + kind + u' ' + name;
    const double value = std::clamp(balance, -1.0, 1.0);
    m_writes.insert(key, value);
    m_flush.start();
    expect(key, value, true);
}

void Audio::setProfile(const QString& device, int index)
{
    const Device* d = m_core->snapshot().device(device);
    const QString key = QStringLiteral("profile ") + device;
    if (!d || index < 0 || !m_core->setProfile(d->id, quint32(index))) {
        fail(key);
        return;
    }
    expect(key, index, true, profileMs);
}

QString Audio::testSoundOutput() const
{
    return m_testSound->playing();
}

void Audio::playTestSound(const QString& output)
{
    if (endpoint(QStringLiteral("output"), output)) {
        m_testSound->play(output);
    }
}

void Audio::stopTestSound()
{
    m_testSound->stop();
}

QList<double> Audio::channelsFor(const Endpoint& e, const QString& kind) const
{
    const QString suffix = kind + u' ' + e.name;
    const auto volume = m_pending.constFind(QStringLiteral("volume ") + suffix);
    const double v = volume != m_pending.constEnd() ? volume->value.toDouble() : std::min(e.volume(), 1.0);
    QList<double> channels = scaledChannels(e.channelVolumes, v, e.shape);
    const auto balance = m_pending.constFind(QStringLiteral("balance ") + suffix);
    if (balance != m_pending.constEnd()) {
        channels = withStereoBalance(volumePositions(e), channels, balance->value.toDouble());
    }
    return channels;
}

void Audio::setAppVolume(const QString& key, double volume)
{
    if (appStreams(key, Direction::Output).isEmpty()) {
        return;
    }
    const QString pendingKey = QStringLiteral("volume app ") + key;
    const double value = VolumeSteps::snap(volume, VolumeSteps::evenGrid);
    m_writes.insert(pendingKey, value);
    m_flush.start();
    expect(pendingKey, value, true);
}

void Audio::stepAppVolume(const QString& key, int direction)
{
    const QVariantMap app = findByKey(m_apps, QStringLiteral("key"), key);
    if (app.isEmpty() || direction == 0) {
        return;
    }
    setAppVolume(key, VolumeSteps::step(app.value(QStringLiteral("volume")).toDouble(), direction, VolumeSteps::evenGrid));
    if (app.value(QStringLiteral("muted")).toBool()) {
        setAppMuted(key, false); // like the system volume
    }
}

void Audio::setAppMuted(const QString& key, bool muted)
{
    const QList<const Stream*> streams = appStreams(key, Direction::Output);
    if (streams.isEmpty()) {
        return;
    }
    for (const Stream* s : streams) {
        m_core->setMuted(s->id, muted);
    }
    expect(QStringLiteral("mute app ") + key, muted, true);
}

void Audio::setAppOutput(const QString& key, const QString& output)
{
    const QList<const Stream*> streams = appStreams(key, Direction::Output);
    if (streams.isEmpty() || (!output.isEmpty() && !endpoint(QStringLiteral("output"), output))) {
        return;
    }
    for (const Stream* s : streams) {
        m_core->setTarget(s->id, output);
    }
    expect(QStringLiteral("output app ") + key, output, true);
}

void Audio::setAppInput(const QString& key, const QString& input)
{
    const QList<const Stream*> streams = appStreams(key, Direction::Input);
    if (streams.isEmpty() || (!input.isEmpty() && !endpoint(QStringLiteral("input"), input))) {
        return;
    }
    for (const Stream* s : streams) {
        m_core->setTarget(s->id, input);
    }
    expect(QStringLiteral("input app ") + key, input, true);
}

void Audio::resetApps()
{
    QSet<QString> keys;
    for (const Stream& s : m_core->snapshot().streams) {
        if (s.isApp() && !s.target.isEmpty()) {
            m_core->setTarget(s.id, QString());
            keys.insert((s.direction == Direction::Output ? QStringLiteral("output app ") : QStringLiteral("input app "))
                + s.app.key);
        }
    }
    for (const QString& key : std::as_const(keys)) {
        expect(key, QString(), true);
    }
}

QString Audio::iconName() const
{
    if (m_muted || m_volume <= 0.0) {
        return QStringLiteral("audio-volume-muted");
    }
    return m_volume < 0.34 ? QStringLiteral("audio-volume-low")
        : m_volume < 0.67  ? QStringLiteral("audio-volume-medium")
                           : QStringLiteral("audio-volume-high");
}

void Audio::watchBattery(const QString& path)
{
    if (m_batteryWatched.contains(path)) {
        return;
    }
    m_batteryWatched.insert(path);
    // Each watch has a distinct version. A reconnect must not accept a
    // reply from a previous incarnation of the same BlueZ path.
    const quint64 requestVersion = ++m_nextBatteryVersion;
    m_batteryVersions.insert(path, requestVersion);
    const QString bluez = QStringLiteral("org.bluez");
    const QString properties = QStringLiteral("org.freedesktop.DBus.Properties");
    QDBusConnection bus = QDBusConnection::systemBus();
    bus.connect(bluez, path, properties, QStringLiteral("PropertiesChanged"), this,
        SLOT(onBluetoothProperties(QDBusMessage)));
    QDBusMessage get = QDBusMessage::createMethodCall(bluez, path, properties, QStringLiteral("Get"));
    get << QStringLiteral("org.bluez.Battery1") << QStringLiteral("Percentage");
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(get, 1000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, path, requestVersion] {
        const QDBusPendingReply<QDBusVariant> reply = *watcher;
        watcher->deleteLater();
        // PropertiesChanged may have supplied a newer value while Get was
        // in flight; a disconnected/reconnected device may also reuse path.
        if (m_batteryVersions.value(path, 0) != requestVersion) {
            return;
        }
        bool ok = false;
        const int percent = reply.isError() ? -1 : reply.value().variant().toInt(&ok);
        if (ok && percent >= 0 && percent <= 100) {
            m_batteries.insert(path, percent);
            update();
        }
    });
}

void Audio::onBluetoothProperties(const QDBusMessage& message)
{
    if (!m_batteryWatched.contains(message.path())) {
        return;
    }
    const QList<QVariant> args = message.arguments();
    if (args.value(0).toString() != QLatin1String("org.bluez.Battery1")) {
        return;
    }
    bool ok = false;
    const QVariant changed = args.value(1);
    const QVariantMap properties = changed.canConvert<QDBusArgument>()
        ? qdbus_cast<QVariantMap>(changed.value<QDBusArgument>())
        : changed.toMap();
    if (!properties.contains(QStringLiteral("Percentage"))) {
        return;
    }
    const int percent = properties.value(QStringLiteral("Percentage")).toInt(&ok);
    if (ok && percent >= 0 && percent <= 100) {
        m_batteryVersions.insert(message.path(), ++m_nextBatteryVersion);
        m_batteries.insert(message.path(), percent);
        update();
    }
}

QString Audio::appKey(const QString& desktopId) const
{
    if (desktopId.isEmpty()) {
        return {};
    }
    return findByKey(m_apps, QStringLiteral("desktopId"), desktopId).value(QStringLiteral("key")).toString();
}

void Audio::flush()
{
    const auto writes = std::exchange(m_writes, {});
    // An endpoint's volume and balance go out together, once.
    QHash<QString, bool> endpoints;
    for (auto it = writes.begin(); it != writes.end(); ++it) {
        const QStringList parts = it.key().split(u' ');
        if (parts.size() < 3) {
            continue;
        }
        const QString name = it.key().section(u' ', 2);
        bool ok = false;
        if (parts[1] == QLatin1String("app")) {
            for (const Stream* s : appStreams(name, Direction::Output)) {
                ok = m_core->setVolume(s->id, it.value()) || ok;
            }
        } else if (const Endpoint* e = endpoint(parts[1], name)) {
            const QString target = parts[1] + u' ' + name;
            if (!endpoints.contains(target)) {
                endpoints.insert(target, m_core->setChannelVolumes(e->id, channelsFor(*e, parts[1])));
            }
            ok = endpoints.value(target);
        }
        if (!ok) {
            m_pending.remove(it.key()); // the server's value stands
        }
    }
    update();
}

// ------------------------------------------------------------------- view --

void Audio::update()
{
    const Snapshot& s = m_core->snapshot();
    // An error raised while WirePlumber had no default metadata is no
    // longer relevant once defaults become available again.
    if (s.defaultsKnown && !m_defaultsKnown) {
        m_errors.remove(QStringLiteral("default input"));
        m_errors.remove(QStringLiteral("default output"));
    }
    m_defaultsKnown = s.defaultsKnown;

    // An app's value, from its streams in one direction.
    const auto appValue = [&](const QString& what, const QString& key) -> QVariant {
        const QList<const Stream*> streams
            = appStreams(key, what == QLatin1String("input") ? Direction::Input : Direction::Output);
        if (streams.isEmpty()) {
            return {};
        }
        if (what == QLatin1String("volume")) {
            double loudest = 0.0;
            for (const Stream* st : streams) {
                loudest = std::max(loudest, st->volume());
            }
            return std::min(loudest, 1.0);
        }
        if (what == QLatin1String("mute")) {
            return std::all_of(streams.begin(), streams.end(), [](const Stream* st) { return st->muted; });
        }
        for (const Stream* st : streams) {
            if (!st->target.isEmpty()) {
                return st->target;
            }
        }
        return QString();
    };
    // What the server says for a pending key.
    const auto serverValue = [&](const QString& key) -> QVariant {
        const QStringList parts = key.split(u' ');
        const QString name = key.section(u' ', 2);
        if (parts.value(0) == QLatin1String("profile")) {
            const Device* d = s.device(key.section(u' ', 1));
            return d ? QVariant(int(d->profile)) : QVariant();
        }
        if (parts.value(0) == QLatin1String("default")) {
            return parts.value(1) == QLatin1String("output") ? s.defaultOutput : s.defaultInput;
        }
        if (parts.value(1) == QLatin1String("app")) {
            return appValue(parts.value(0), name);
        }
        const Endpoint* e = endpoint(parts.value(1), name);
        if (!e) {
            return {};
        }
        if (parts.value(0) == QLatin1String("volume")) {
            return std::min(e->volume(), 1.0);
        }
        if (parts.value(0) == QLatin1String("balance")) {
            return balanceOf(*e);
        }
        return e->muted;
    };

    // Confirmed or expired actions.
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (same(serverValue(it.key()), it->value)) {
            m_errors.remove(it.key());
            it = m_pending.erase(it);
        } else if (it->since.elapsed() > it->timeoutMs) {
            if (!it->optimistic) {
                m_errors.insert(it.key());
            }
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
    if (m_pending.isEmpty()) {
        m_expiry.stop();
    }
    // A value just set, or the server's.
    const auto shown = [&](const QString& key, const QVariant& server) {
        const auto it = m_pending.constFind(key);
        return it != m_pending.constEnd() && it->optimistic ? it->value : server;
    };
    const auto pending = [&](const QString& key) {
        const auto it = m_pending.constFind(key);
        return it != m_pending.constEnd() && !it->optimistic;
    };

    const QString defaultOutput = shown(QStringLiteral("default output"), s.defaultOutput).toString();
    const QString defaultInput = s.defaultInput;
    QVariantList outputs;
    QVariantList inputs;
    QSet<QString> activeBatteryPaths;
    double volume = 0.0;
    bool muted = false;
    bool inputMuted = false;
    for (const Endpoint& e : s.endpoints) {
        const QString kind = kindOf(e.direction);
        const bool output = e.direction == Direction::Output;
        const bool isDefault = e.name == (output ? defaultOutput : defaultInput);
        if (output && !e.available && !isDefault) {
            continue; // like Windows: what isn't plugged in isn't offered
        }
        const QString suffix = kind + u' ' + e.name;
        // A Bluetooth headset: its BlueZ object, given by PipeWire or made
        // from its address.
        QString bluez = e.bluezPath;
        if (bluez.isEmpty() && !e.bluetoothAddress.isEmpty()) {
            bluez = QStringLiteral("/org/bluez/hci0/dev_") + QString(e.bluetoothAddress).replace(u':', u'_');
        }
        if (!bluez.isEmpty()) {
            activeBatteryPaths.insert(bluez);
            watchBattery(bluez);
        }
        const double v = shown(QStringLiteral("volume ") + suffix, std::min(e.volume(), 1.0)).toDouble();
        const bool m = shown(QStringLiteral("mute ") + suffix, e.muted).toBool();
        const Device* device = s.device(e.device);
        const bool stereo = hasStereo(volumePositions(e));
        const QVariantMap map {
            { QStringLiteral("name"), e.name },
            { QStringLiteral("description"), e.description },
            { QStringLiteral("icon"), e.icon },
            { QStringLiteral("volume"), v },
            { QStringLiteral("muted"), m },
            { QStringLiteral("isDefault"), isDefault },
            { QStringLiteral("intervals"), VolumeSteps::intervals(e.step) },
            { QStringLiteral("channels"), int(e.positions.isEmpty() ? e.channelVolumes.size() : e.positions.size()) },
            { QStringLiteral("battery"), bluez.isEmpty() ? -1 : m_batteries.value(bluez, -1) },
            { QStringLiteral("mutePending"), pending(QStringLiteral("mute ") + suffix) },
            { QStringLiteral("muteError"), m_errors.contains(QStringLiteral("mute ") + suffix) },
            { QStringLiteral("defaultPending"), pending(QStringLiteral("default ") + kind) },
            { QStringLiteral("defaultError"), m_errors.contains(QStringLiteral("default ") + kind) },
            { QStringLiteral("device"), device ? device->name : QString() },
            { QStringLiteral("stereo"), stereo },
            { QStringLiteral("balance"), stereo ? shown(QStringLiteral("balance ") + suffix, balanceOf(e)).toDouble() : 0.0 },
        };
        (output ? outputs : inputs).append(map);
        if (isDefault && output) {
            volume = v;
            muted = m;
        } else if (isDefault) {
            inputMuted = m;
        }
    }

    // A removed Bluetooth device can later reuse the very same D-Bus path.
    // Disconnect its subscription, forget the old percent, and let the next
    // appearance request fresh state from BlueZ.
    const QSet<QString> watchedPaths = m_batteryWatched;
    for (const QString& path : watchedPaths) {
        if (activeBatteryPaths.contains(path)) {
            continue;
        }
        QDBusConnection::systemBus().disconnect(QStringLiteral("org.bluez"), path,
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"),
            this, SLOT(onBluetoothProperties(QDBusMessage)));
        m_batteryWatched.remove(path);
        m_batteryVersions.remove(path);
        m_batteries.remove(path);
    }

    QVariantList devices;
    for (const Device& d : s.devices) {
        const QString key = QStringLiteral("profile ") + d.name;
        const int profile = shown(key, int(d.profile)).toInt();
        QVariantList profiles;
        bool off = false;
        for (const Profile& p : d.profiles) {
            const bool active = int(p.index) == profile;
            off = off || (active && p.name == QLatin1String("off"));
            if (!p.available && !active) {
                continue; // like Windows: what can't play isn't offered
            }
            profiles.append(QVariantMap {
                { QStringLiteral("index"), int(p.index) },
                { QStringLiteral("name"), p.name },
                { QStringLiteral("description"), p.description },
                { QStringLiteral("available"), p.available },
                { QStringLiteral("priority"), int(p.priority) },
            });
        }
        QStringList outputNames;
        QStringList inputNames;
        for (const Endpoint& e : s.endpoints) {
            if (e.device == d.id) {
                (e.direction == Direction::Output ? outputNames : inputNames).append(e.name);
            }
        }
        devices.append(QVariantMap {
            { QStringLiteral("name"), d.name },
            { QStringLiteral("description"), d.description },
            { QStringLiteral("icon"), d.icon },
            { QStringLiteral("bluetooth"), d.bluetooth },
            { QStringLiteral("off"), off },
            { QStringLiteral("profile"), profile },
            { QStringLiteral("profiles"), profiles },
            { QStringLiteral("profilePending"), m_pending.contains(key) },
            { QStringLiteral("profileError"), m_errors.contains(key) },
            { QStringLiteral("outputs"), outputNames },
            { QStringLiteral("inputs"), inputNames },
        });
    }

    QVariantList apps;
    QSet<QString> seen;
    for (const Stream& st : s.streams) {
        if (!st.isApp() || seen.contains(st.app.key)) {
            continue;
        }
        seen.insert(st.app.key);
        const QString key = st.app.key;
        AppInfo info = m_resolver ? m_resolver(st.app) : AppInfo {};
        if (info.name.isEmpty()) {
            info.name = !st.app.name.isEmpty() ? st.app.name : key;
        }
        if (info.icon.isEmpty()) {
            info.icon = !st.app.iconName.isEmpty() ? st.app.iconName : QStringLiteral("applications-multimedia");
        }
        const QVariant playing = appValue(QStringLiteral("output"), key);
        const QVariant recording = appValue(QStringLiteral("input"), key);
        const QString output = shown(QStringLiteral("output app ") + key, playing).toString();
        const QString input = shown(QStringLiteral("input app ") + key, recording).toString();
        const Endpoint* outputEndpoint = output.isEmpty() ? nullptr : s.endpoint(output);
        const Endpoint* inputEndpoint = input.isEmpty() ? nullptr : s.endpoint(input);
        apps.append(QVariantMap {
            { QStringLiteral("key"), key },
            { QStringLiteral("name"), info.name },
            { QStringLiteral("icon"), info.icon },
            { QStringLiteral("desktopId"), info.desktopId },
            { QStringLiteral("playing"), playing.isValid() },
            { QStringLiteral("recording"), recording.isValid() },
            { QStringLiteral("volume"), shown(QStringLiteral("volume app ") + key, appValue(QStringLiteral("volume"), key)).toDouble() },
            { QStringLiteral("muted"), shown(QStringLiteral("mute app ") + key, appValue(QStringLiteral("mute"), key)).toBool() },
            { QStringLiteral("output"), output },
            { QStringLiteral("outputDescription"), outputEndpoint ? outputEndpoint->description : output },
            { QStringLiteral("input"), input },
            { QStringLiteral("inputDescription"), inputEndpoint ? inputEndpoint->description : input },
        });
    }

    if (outputs != m_outputs || inputs != m_inputs || apps != m_apps || devices != m_devices
        || defaultOutput != m_defaultOutput
        || defaultInput != m_defaultInput || volume != m_volume || muted != m_muted || inputMuted != m_inputMuted) {
        m_outputs = outputs;
        m_inputs = inputs;
        m_apps = apps;
        m_devices = devices;
        m_defaultOutput = defaultOutput;
        m_defaultInput = defaultInput;
        m_volume = volume;
        m_muted = muted;
        m_inputMuted = inputMuted;
        emit changed();
    }
}

} // namespace vela::audio
