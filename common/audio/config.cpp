// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include <spa/utils/json.h>

#include "config.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVariant>

#include <algorithm>

Q_DECLARE_LOGGING_CATEGORY(lcAudio)

namespace vela::audio {

namespace {

// WirePlumber saves its state about a second after a change, and not when
// it exits (docs/audio.md §8).
constexpr int quietMs = 1500;

const QString wireplumberUnit = QStringLiteral("wireplumber.service");
const QString pipewireUnit = QStringLiteral("pipewire.service");

const char* const header = "# Written by Vela's Settings (docs/audio.md §8): it rewrites this file\n"
                           "# whole, so changes made here are lost. Put your own rules in a file\n"
                           "# of your own.\n";

QVariant toVariant(spa_json* iter, const char* value, int len)
{
    if (spa_json_is_object(value, len)) {
        spa_json sub;
        spa_json_enter(iter, &sub);
        QVariantMap map;
        char key[1024];
        const char* v = nullptr;
        int l = 0;
        while ((l = spa_json_object_next(&sub, key, sizeof(key), &v)) > 0) {
            map.insert(QString::fromUtf8(key), toVariant(&sub, v, l));
        }
        return map;
    }
    if (spa_json_is_array(value, len)) {
        spa_json sub;
        spa_json_enter(iter, &sub);
        QVariantList list;
        const char* v = nullptr;
        int l = 0;
        while ((l = spa_json_next(&sub, &v)) > 0) {
            list.append(toVariant(&sub, v, l));
        }
        return list;
    }
    if (spa_json_is_null(value, len)) {
        return {};
    }
    int number = 0;
    if (spa_json_is_int(value, len) && spa_json_parse_int(value, len, &number) > 0) {
        return number;
    }
    // Strings, quoted or not (SPA-JSON's bare words).
    QByteArray text(len + 1, '\0');
    if (spa_json_parse_stringn(value, len, text.data(), int(text.size())) < 0) {
        return {};
    }
    return QString::fromUtf8(text.constData());
}

QString quoted(const QString& text)
{
    QString out = QStringLiteral("\"");
    for (const QChar c : text) {
        if (c == u'"' || c == u'\\') {
            out += u'\\';
            out += c;
        } else if (c.unicode() < 0x20) {
            out += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
        } else {
            out += c;
        }
    }
    return out + u'"';
}

QString rateList(const QList<quint32>& rates)
{
    QStringList items;
    for (quint32 rate : rates) {
        items.append(QString::number(rate));
    }
    return QStringLiteral("[ ") + items.join(u' ') + QStringLiteral(" ]");
}

QString stringList(const QStringList& strings)
{
    QStringList items;
    for (const QString& s : strings) {
        items.append(quoted(s));
    }
    return QStringLiteral("[ ") + items.join(u' ') + QStringLiteral(" ]");
}

QList<quint32> toRates(const QVariant& value)
{
    QList<quint32> out;
    for (const QVariant& item : value.toList()) {
        bool ok = false;
        const quint32 rate = item.toUInt(&ok);
        if (ok && rate > 0) {
            out.append(rate);
        }
    }
    return out;
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QString& text)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        qCWarning(lcAudio) << "can't create the directory of" << path;
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8()) < 0 || !file.commit()) {
        qCWarning(lcAudio) << "can't write" << path << file.errorString();
        return false;
    }
    return true;
}

void restartWithSystemd(const QString& unit)
{
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
        QStringLiteral("/org/freedesktop/systemd1"), QStringLiteral("org.freedesktop.systemd1.Manager"),
        QStringLiteral("RestartUnit"));
    call << unit << QStringLiteral("replace");
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [unit](QDBusPendingCallWatcher* w) {
        if (w->isError()) {
            qCWarning(lcAudio) << "can't restart" << unit << w->error().message();
        }
        w->deleteLater();
    });
}

} // namespace

Config::Config(QObject* parent)
    : QObject(parent)
    , m_restarter(restartWithSystemd)
{
    m_restart.setSingleShot(true);
    connect(&m_restart, &QTimer::timeout, this, &Config::restartNow);
}

QString Config::wireplumberFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/wireplumber/wireplumber.conf.d/90-vela.conf");
}

QString Config::pipewireFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/pipewire/pipewire.conf.d/90-vela.conf");
}

QVariant Config::parse(const QByteArray& text)
{
    spa_json root;
    if (text.trimmed().isEmpty() || spa_json_begin_object_relax(&root, text.constData(), size_t(text.size())) <= 0) {
        return QVariantMap();
    }
    QVariantMap map;
    char key[1024];
    const char* value = nullptr;
    int len = 0;
    while ((len = spa_json_object_next(&root, key, sizeof(key), &value)) > 0) {
        map.insert(QString::fromUtf8(key), toVariant(&root, value, len));
    }
    return map;
}

// ---------------------------------------------------------- WirePlumber --

QList<DeviceRule> Config::rules() const
{
    QList<DeviceRule> out;
    const QVariantMap file = parse(readFile(wireplumberFile())).toMap();
    for (const QString& section : { QStringLiteral("monitor.alsa.rules"), QStringLiteral("monitor.bluez.rules") }) {
        for (const QVariant& item : file.value(section).toList()) {
            const QVariantMap rule = item.toMap();
            const QVariantList matches = rule.value(QStringLiteral("matches")).toList();
            const QVariantMap props
                = rule.value(QStringLiteral("actions")).toMap().value(QStringLiteral("update-props")).toMap();
            if (matches.size() != 1 || props.isEmpty()) {
                continue;
            }
            DeviceRule r;
            r.node = matches.first().toMap().value(QStringLiteral("node.name")).toString();
            if (r.node.isEmpty()) {
                continue;
            }
            r.bluetooth = section == QLatin1String("monitor.bluez.rules");
            r.description = props.value(QStringLiteral("node.description")).toString();
            r.allowedRates = toRates(props.value(QStringLiteral("audio.allowed-rates")));
            r.format = props.value(QStringLiteral("audio.format")).toString();
            r.offeredRates = toRates(props.value(QStringLiteral("vela.audio.rates")));
            r.offeredFormats = props.value(QStringLiteral("vela.audio.formats")).toStringList();
            out.append(r);
        }
    }
    return out;
}

DeviceRule Config::rule(const QString& node) const
{
    const QList<DeviceRule> all = rules();
    const auto it = std::find_if(all.begin(), all.end(), [&](const DeviceRule& r) { return r.node == node; });
    if (it != all.end()) {
        return *it;
    }
    DeviceRule r;
    r.node = node;
    return r;
}

bool Config::setRule(const DeviceRule& rule)
{
    QList<DeviceRule> all = rules();
    all.erase(std::remove_if(all.begin(), all.end(), [&](const DeviceRule& r) { return r.node == rule.node; }),
        all.end());
    if (!rule.isEmpty()) {
        all.append(rule);
    }
    std::sort(all.begin(), all.end(), [](const DeviceRule& a, const DeviceRule& b) { return a.node < b.node; });

    QString text = QString::fromUtf8(header);
    for (const bool bluetooth : { false, true }) {
        QString section;
        for (const DeviceRule& r : std::as_const(all)) {
            if (r.bluetooth != bluetooth) {
                continue;
            }
            QStringList props;
            if (!r.description.isEmpty()) {
                props.append(QStringLiteral("node.description = ") + quoted(r.description));
            }
            if (!r.allowedRates.isEmpty()) {
                props.append(QStringLiteral("audio.allowed-rates = ") + rateList(r.allowedRates));
            }
            if (!r.format.isEmpty()) {
                props.append(QStringLiteral("audio.format = ") + quoted(r.format));
            }
            // What it offered is only needed while it is restricted.
            if (r.restricts() && !r.offeredRates.isEmpty()) {
                props.append(QStringLiteral("vela.audio.rates = ") + rateList(r.offeredRates));
            }
            if (r.restricts() && !r.offeredFormats.isEmpty()) {
                props.append(QStringLiteral("vela.audio.formats = ") + stringList(r.offeredFormats));
            }
            section += QStringLiteral("  {\n    matches = [ { node.name = ") + quoted(r.node)
                + QStringLiteral(" } ]\n    actions = {\n      update-props = {\n");
            for (const QString& p : std::as_const(props)) {
                section += QStringLiteral("        ") + p + u'\n';
            }
            section += QStringLiteral("      }\n    }\n  }\n");
        }
        if (!section.isEmpty()) {
            text += u'\n' + (bluetooth ? QStringLiteral("monitor.bluez.rules") : QStringLiteral("monitor.alsa.rules"))
                + QStringLiteral(" = [\n") + section + QStringLiteral("]\n");
        }
    }
    if (!writeFile(wireplumberFile(), text)) {
        return false;
    }
    restartWireplumber();
    return true;
}

// ------------------------------------------------------------- PipeWire --

bool Config::hasPipewireFile() const
{
    return QFileInfo::exists(pipewireFile());
}

QList<quint32> Config::pipewireRates() const
{
    const QVariantMap file = parse(readFile(pipewireFile())).toMap();
    return toRates(
        file.value(QStringLiteral("context.properties")).toMap().value(QStringLiteral("default.clock.allowed-rates")));
}

bool Config::setPipewireRates(const QList<quint32>& rates)
{
    if (rates.isEmpty()) {
        return false;
    }
    return writeFile(pipewireFile(),
        QString::fromUtf8(header) + QStringLiteral("\ncontext.properties = {\n    default.clock.allowed-rates = ")
            + rateList(rates) + QStringLiteral("\n}\n"));
}

// ------------------------------------------------------------- restarts --

void Config::noteChange()
{
    m_lastChange.start();
}

void Config::restartWireplumber()
{
    schedule(wireplumberUnit);
}

void Config::restartAudio()
{
    schedule(pipewireUnit);
}

void Config::schedule(const QString& unit)
{
    // PipeWire's restart takes WirePlumber with it.
    if (!m_restart.isActive() || unit == pipewireUnit) {
        m_unit = unit;
    }
    const qint64 since = m_lastChange.isValid() ? m_lastChange.elapsed() : quietMs;
    const bool wasPending = m_restart.isActive();
    m_restart.start(int(std::max<qint64>(0, quietMs - since)));
    if (!wasPending) {
        emit restartPendingChanged();
    }
}

void Config::restartNow()
{
    // A change made meanwhile still needs its time.
    if (m_lastChange.isValid() && m_lastChange.elapsed() < quietMs) {
        m_restart.start(int(quietMs - m_lastChange.elapsed()));
        return;
    }
    const QString unit = std::exchange(m_unit, QString());
    emit restartPendingChanged();
    if (m_restarter && !unit.isEmpty()) {
        m_restarter(unit);
    }
}

} // namespace vela::audio
