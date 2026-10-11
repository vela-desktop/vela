// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// WirePlumber first: GLib's headers have fields named "signals", which Qt
// turns into a keyword.
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <spa/param/param.h>
#include <wp/wp.h>

#include "core.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QThread>

#include <mutex>

Q_LOGGING_CATEGORY(lcAudio, "vela.audio")

namespace vela::audio {

namespace {

// Reconnection delays, in ms: PipeWire restarting, or not there yet.
constexpr int backoffs[] = { 500, 1000, 2000, 4000, 5000 };

const char* const plugins[] = { "libwireplumber-module-mixer-api", "libwireplumber-module-default-nodes-api" };

QString prop(gpointer object, const char* key)
{
    return QString::fromUtf8(wp_pipewire_object_get_property(WP_PIPEWIRE_OBJECT(object), key));
}

bool flag(gpointer object, const char* key)
{
    const char* value = wp_pipewire_object_get_property(WP_PIPEWIRE_OBJECT(object), key);
    return value && (g_strcmp0(value, "true") == 0 || g_strcmp0(value, "1") == 0);
}

quint32 boundId(gpointer object)
{
    return wp_proxy_get_bound_id(WP_PROXY(object));
}

// "[ FL FR ]", "FL,FR" -> FL, FR
QStringList parsePositions(const QString& text)
{
    QString list = text;
    list.remove(u'[').remove(u']').replace(u',', u' ');
    return list.split(u' ', Qt::SkipEmptyParts);
}

// A theme icon for an endpoint without one of its own.
QString iconFor(Direction direction, const QString& formFactor, const QString& bus, const QString& name)
{
    if (formFactor == QLatin1String("headset") || formFactor == QLatin1String("hands-free")
        || formFactor == QLatin1String("handset")) {
        return QStringLiteral("audio-headset");
    }
    if (formFactor == QLatin1String("headphone")) {
        return QStringLiteral("audio-headphones");
    }
    if (formFactor == QLatin1String("webcam")) {
        return QStringLiteral("camera-web");
    }
    if (name.contains(QLatin1String("hdmi"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("displayport"), Qt::CaseInsensitive)) {
        return QStringLiteral("video-display");
    }
    if (direction == Direction::Input) {
        return QStringLiteral("audio-input-microphone");
    }
    return bus == QLatin1String("bluetooth") ? QStringLiteral("audio-headphones") : QStringLiteral("audio-speakers");
}

// get-volume's answer: {mute, step, channelVolumes: {"0": {volume, channel}...}}
void readVolume(GVariant* volume, QList<double>& channels, QStringList& positions, bool& muted, double* step)
{
    gboolean mute = FALSE;
    if (g_variant_lookup(volume, "mute", "b", &mute)) {
        muted = mute;
    }
    gdouble value = 0.0;
    if (step && g_variant_lookup(volume, "step", "d", &value)) {
        *step = value;
    }
    GVariant* perChannel = g_variant_lookup_value(volume, "channelVolumes", G_VARIANT_TYPE_VARDICT);
    if (!perChannel) {
        return;
    }
    const gsize count = g_variant_n_children(perChannel);
    QList<double> volumes;
    volumes.resize(qsizetype(count));
    QStringList names;
    names.resize(qsizetype(count));
    GVariantIter iter;
    const gchar* index = nullptr;
    GVariant* channel = nullptr;
    g_variant_iter_init(&iter, perChannel);
    while (g_variant_iter_loop(&iter, "{&sv}", &index, &channel)) {
        bool ok = false;
        const int i = QByteArray(index).toInt(&ok);
        if (!ok || i < 0 || gsize(i) >= count) {
            continue;
        }
        gdouble v = 0.0;
        if (g_variant_lookup(channel, "volume", "d", &v)) {
            volumes[i] = v;
        }
        const gchar* name = nullptr;
        if (g_variant_lookup(channel, "channel", "&s", &name)) {
            names[i] = QString::fromUtf8(name);
        }
    }
    g_variant_unref(perChannel);
    channels = volumes;
    // A suspended node may report fewer volumes than it has channels (two,
    // until its format is set): its audio.position is then the layout.
    if (positions.isEmpty() && !names.contains(QString())) {
        positions = names;
    }
}

} // namespace

struct Core::Private {
    Core* q;
    WpCore* core = nullptr;
    WpObjectManager* om = nullptr;
    WpPlugin* mixer = nullptr;
    WpPlugin* defaults = nullptr;
    int pendingPlugins = 0;
    bool installed = false;
    // Callbacks of asynchronous calls carry the token of the connection that
    // made them: one from a connection already torn down finds nothing.
    quintptr token = 0;
    // Each endpoint's balance while its volume was up (by node.name): from
    // silence the volume comes back with the same shape.
    QHash<QString, QList<double>> balances;

    static QHash<quintptr, Private*>& live()
    {
        static QHash<quintptr, Private*> instances;
        return instances;
    }
    static Private* find(gpointer token) { return live().value(quintptr(token)); }

    void schedule() { q->m_rebuild.start(); }

    WpMetadata* metadata() const
    {
        return om ? static_cast<WpMetadata*>(wp_object_manager_lookup(om, WP_TYPE_METADATA, nullptr)) : nullptr;
    }

    // GLib callbacks: d is alive while they are connected (teardown
    // disconnects them all before anything is freed).
    static void onChanged(Private* d) { d->schedule(); }
    static void onMixerChanged(WpPlugin*, guint, Private* d) { d->schedule(); }
    static void onMetadataChanged(WpMetadata*, guint, const gchar*, const gchar*, const gchar*, Private* d)
    {
        d->schedule();
    }
    static void onParamsChanged(WpPipewireObject*, const gchar*, Private* d) { d->schedule(); }
    static void onStateChanged(WpNode*, WpNodeState, WpNodeState, Private* d) { d->schedule(); }
    static void onPropertiesChanged(GObject*, GParamSpec*, Private* d) { d->schedule(); }

    static void onObjectAdded(WpObjectManager*, gpointer object, Private* d)
    {
        if (WP_IS_NODE(object)) {
            g_signal_connect(object, "state-changed", G_CALLBACK(onStateChanged), d);
        }
        if (WP_IS_METADATA(object)) {
            g_signal_connect(object, "changed", G_CALLBACK(onMetadataChanged), d);
        }
        if (WP_IS_PIPEWIRE_OBJECT(object)) {
            g_signal_connect(object, "notify::properties", G_CALLBACK(onPropertiesChanged), d);
            g_signal_connect(object, "params-changed", G_CALLBACK(onParamsChanged), d);
        }
    }
    static void onObjectRemoved(WpObjectManager*, gpointer object, Private* d)
    {
        g_signal_handlers_disconnect_by_data(object, d);
    }

    static void onInstalled(WpObjectManager*, Private* d)
    {
        d->installed = true;
        d->q->m_backoff = 0;
        d->q->rebuild();
    }

    static void onDisconnected(WpCore*, Private* d)
    {
        // Not from inside the core's own signal: tear down from the loop.
        QMetaObject::invokeMethod(
            d->q,
            [q = d->q] {
                q->teardown();
                q->scheduleReconnect();
            },
            Qt::QueuedConnection);
    }

    static void onPluginLoaded(GObject* source, GAsyncResult* result, gpointer token)
    {
        GError* error = nullptr;
        const bool ok = wp_core_load_component_finish(WP_CORE(source), result, &error);
        Private* d = find(token);
        if (!ok) {
            if (d) {
                qCWarning(lcAudio) << "can't load a WirePlumber plugin:" << (error ? error->message : "");
            }
            g_clear_error(&error);
            if (d) {
                d->q->teardown();
                d->q->scheduleReconnect();
            }
            return;
        }
        if (d && --d->pendingPlugins == 0) {
            d->pluginsReady();
        }
    }

    void pluginsReady()
    {
        mixer = wp_plugin_find(core, "mixer-api");
        defaults = wp_plugin_find(core, "default-nodes-api");
        if (!mixer || !defaults) {
            qCWarning(lcAudio) << "WirePlumber's mixer-api or default-nodes-api is missing";
            q->teardown();
            q->scheduleReconnect();
            return;
        }
        g_object_set(mixer, "scale", 1 /* cubic */, nullptr);
        g_signal_connect(mixer, "changed", G_CALLBACK(onMixerChanged), this);
        g_signal_connect_swapped(defaults, "changed", G_CALLBACK(onChanged), this);

        om = wp_object_manager_new();
        wp_object_manager_add_interest(om, WP_TYPE_NODE, WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY, PW_KEY_MEDIA_CLASS,
            "#s", "*Audio*", nullptr);
        wp_object_manager_add_interest(om, WP_TYPE_DEVICE, WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY, PW_KEY_MEDIA_CLASS,
            "=s", "Audio/Device", nullptr);
        wp_object_manager_add_interest(om, WP_TYPE_LINK, nullptr);
        wp_object_manager_add_interest(om, WP_TYPE_METADATA, WP_CONSTRAINT_TYPE_PW_GLOBAL_PROPERTY, "metadata.name",
            "=s", "default", nullptr);
        wp_object_manager_request_object_features(om, WP_TYPE_GLOBAL_PROXY, WP_OBJECT_FEATURES_ALL);
        g_signal_connect(om, "object-added", G_CALLBACK(onObjectAdded), this);
        g_signal_connect(om, "object-removed", G_CALLBACK(onObjectRemoved), this);
        g_signal_connect_swapped(om, "objects-changed", G_CALLBACK(onChanged), this);
        g_signal_connect(om, "installed", G_CALLBACK(onInstalled), this);
        wp_core_install_object_manager(core, om);
    }
};

Core::Core(QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->q = this;
    m_rebuild.setSingleShot(true);
    m_rebuild.setInterval(0);
    connect(&m_rebuild, &QTimer::timeout, this, &Core::rebuild);
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &Core::connectNow);

    static std::once_flag initialized;
    std::call_once(initialized, [] { wp_init(WpInitFlags(WP_INIT_PIPEWIRE | WP_INIT_SPA_TYPES)); });

    if (!eventLoopSupported()) {
        qCWarning(lcAudio) << "Qt isn't running on GLib's main loop (QT_NO_GLIB?): audio is not available";
        return;
    }
    connectNow();
}

Core::~Core()
{
    blockSignals(true); // nobody hears about a model going away with us
    teardown();
}

bool Core::eventLoopSupported()
{
    const QAbstractEventDispatcher* dispatcher = QAbstractEventDispatcher::instance(QThread::currentThread());
    return dispatcher && dispatcher->inherits("QEventDispatcherGlib");
}

void Core::connectNow()
{
    teardown();
    static quintptr tokens = 0;
    d->token = ++tokens;
    Private::live().insert(d->token, d.get());

    const QByteArray app = QCoreApplication::applicationName().toUtf8();
    d->core = wp_core_new(nullptr, nullptr,
        wp_properties_new(PW_KEY_REMOTE_NAME, "[" PW_DEFAULT_REMOTE "-manager," PW_DEFAULT_REMOTE "]",
            PW_KEY_APP_NAME, app.isEmpty() ? "Vela" : app.constData(), nullptr));
    g_signal_connect(d->core, "disconnected", G_CALLBACK(Private::onDisconnected), d.get());
    d->pendingPlugins = int(std::size(plugins));
    for (const char* plugin : plugins) {
        wp_core_load_component(d->core, plugin, "module", nullptr, nullptr, nullptr, Private::onPluginLoaded,
            reinterpret_cast<gpointer>(d->token));
    }
    if (!wp_core_connect(d->core)) {
        teardown();
        scheduleReconnect();
    }
}

void Core::teardown()
{
    Private::live().remove(d->token);
    m_rebuild.stop();
    if (d->om) {
        WpIterator* it = wp_object_manager_new_iterator(d->om);
        GValue value = G_VALUE_INIT;
        while (wp_iterator_next(it, &value)) {
            g_signal_handlers_disconnect_by_data(g_value_get_object(&value), d.get());
            g_value_unset(&value);
        }
        wp_iterator_unref(it);
        g_signal_handlers_disconnect_by_data(d->om, d.get());
        g_clear_object(&d->om);
    }
    for (WpPlugin** plugin : { &d->mixer, &d->defaults }) {
        if (*plugin) {
            g_signal_handlers_disconnect_by_data(*plugin, d.get());
            g_clear_object(plugin);
        }
    }
    if (d->core) {
        g_signal_handlers_disconnect_by_data(d->core, d.get());
        wp_core_disconnect(d->core);
        g_clear_object(&d->core);
    }
    d->installed = false;
    d->pendingPlugins = 0;

    const bool wasConnected = m_snapshot.connected;
    const bool hadData = wasConnected || !m_snapshot.endpoints.isEmpty() || !m_snapshot.streams.isEmpty();
    m_snapshot = {};
    if (hadData) {
        emit changed();
    }
    if (wasConnected) {
        emit connectedChanged();
    }
}

void Core::scheduleReconnect()
{
    if (!m_reconnect.isActive()) {
        m_reconnect.start(backoffs[std::min<int>(m_backoff++, int(std::size(backoffs)) - 1)]);
    }
}

void Core::rebuild()
{
    if (!d->om || !d->installed) {
        return;
    }
    Snapshot s;
    s.connected = true;

    WpMetadata* metadata = d->metadata();
    s.defaultsKnown = metadata != nullptr;
    guint defaultSink = SPA_ID_INVALID;
    guint defaultSource = SPA_ID_INVALID;
    if (metadata) {
        g_signal_emit_by_name(d->defaults, "get-default-node", "Audio/Sink", &defaultSink);
        g_signal_emit_by_name(d->defaults, "get-default-node", "Audio/Source", &defaultSource);
    }

    // Links: node -> the nodes it sends to, and the reverse.
    QMultiHash<quint32, quint32> sendsTo;
    QMultiHash<quint32, quint32> receivesFrom;
    {
        WpIterator* it = wp_object_manager_new_filtered_iterator(d->om, WP_TYPE_LINK, nullptr);
        GValue value = G_VALUE_INIT;
        while (wp_iterator_next(it, &value)) {
            gpointer link = g_value_get_object(&value);
            bool okOut = false;
            bool okIn = false;
            const quint32 out = prop(link, PW_KEY_LINK_OUTPUT_NODE).toUInt(&okOut);
            const quint32 in = prop(link, PW_KEY_LINK_INPUT_NODE).toUInt(&okIn);
            if (okOut && okIn && !sendsTo.contains(out, in)) {
                sendsTo.insert(out, in);
                receivesFrom.insert(in, out);
            }
            g_value_unset(&value);
        }
        wp_iterator_unref(it);
    }

    QHash<quint32, QString> nodeClasses;
    WpIterator* it = wp_object_manager_new_filtered_iterator(d->om, WP_TYPE_NODE, nullptr);
    GValue value = G_VALUE_INIT;
    while (wp_iterator_next(it, &value)) {
        gpointer node = g_value_get_object(&value);
        const quint32 id = boundId(node);
        const QString mediaClass = prop(node, PW_KEY_MEDIA_CLASS);
        nodeClasses.insert(id, mediaClass);

        QList<double> channels;
        QStringList positions = parsePositions(prop(node, "audio.position"));
        bool muted = false;
        double step = 0.0;
        GVariant* volume = nullptr;
        g_signal_emit_by_name(d->mixer, "get-volume", id, &volume);
        if (volume) {
            readVolume(volume, channels, positions, muted, &step);
            g_variant_unref(volume);
        }

        const bool output = mediaClass.startsWith(QLatin1String("Audio/Sink"));
        const bool input = mediaClass.startsWith(QLatin1String("Audio/Source"));
        if (output || input) {
            Endpoint e;
            e.id = id;
            e.serial = prop(node, PW_KEY_OBJECT_SERIAL).toULongLong();
            e.name = prop(node, PW_KEY_NODE_NAME);
            e.description = prop(node, PW_KEY_NODE_DESCRIPTION);
            if (e.description.isEmpty()) {
                e.description = prop(node, PW_KEY_NODE_NICK);
            }
            if (e.description.isEmpty()) {
                e.description = e.name;
            }
            e.direction = output ? Direction::Output : Direction::Input;
            e.channelVolumes = channels;
            e.positions = positions;
            e.muted = muted;
            e.step = step;
            e.isDefault = id == (output ? defaultSink : defaultSource);
            e.bluetoothAddress = prop(node, "api.bluez5.address");

            QString icon = prop(node, "device.icon-name");
            QString formFactor = prop(node, PW_KEY_DEVICE_FORM_FACTOR);
            QString bus = prop(node, PW_KEY_DEVICE_BUS);
            bool okDevice = false;
            e.device = prop(node, PW_KEY_DEVICE_ID).toUInt(&okDevice);
            gpointer device = okDevice
                ? wp_object_manager_lookup(d->om, WP_TYPE_DEVICE, WP_CONSTRAINT_TYPE_G_PROPERTY, "bound-id", "=u",
                      e.device, nullptr)
                : nullptr;
            if (device) {
                if (icon.isEmpty()) {
                    icon = prop(device, "device.icon-name");
                }
                if (formFactor.isEmpty()) {
                    formFactor = prop(device, PW_KEY_DEVICE_FORM_FACTOR);
                }
                if (bus.isEmpty()) {
                    bus = prop(device, PW_KEY_DEVICE_BUS);
                }
                if (e.bluetoothAddress.isEmpty()) {
                    e.bluetoothAddress = prop(device, "api.bluez5.address");
                }
                e.bluezPath = prop(device, "api.bluez5.path");
                // Unplugged: the route of this node's device says so.
                bool okRoute = false;
                const int routeDevice = prop(node, "card.profile.device").toInt(&okRoute);
                WpIterator* routes = okRoute ? wp_pipewire_object_enum_params_sync(WP_PIPEWIRE_OBJECT(device), "Route", nullptr) : nullptr;
                GValue route = G_VALUE_INIT;
                while (routes && wp_iterator_next(routes, &route)) {
                    gint32 r = -1;
                    guint32 available = SPA_PARAM_AVAILABILITY_unknown;
                    if (wp_spa_pod_get_object(static_cast<WpSpaPod*>(g_value_get_boxed(&route)), nullptr, "device", "i",
                            &r, "available", "?I", &available, nullptr)
                        && r == routeDevice) {
                        e.available = available != SPA_PARAM_AVAILABILITY_no;
                    }
                    g_value_unset(&route);
                }
                if (routes) {
                    wp_iterator_unref(routes);
                }
                g_object_unref(device);
            }
            e.icon = !icon.isEmpty() ? icon : iconFor(e.direction, formFactor, bus, e.name);
            if (e.volume() > 0.0) {
                d->balances.insert(e.name, balanceOf(e.channelVolumes));
            }
            s.endpoints.append(e);
        } else if (mediaClass == QLatin1String("Stream/Output/Audio")
            || mediaClass == QLatin1String("Stream/Input/Audio")) {
            Stream st;
            st.id = id;
            st.serial = prop(node, PW_KEY_OBJECT_SERIAL).toULongLong();
            st.direction = mediaClass.startsWith(QLatin1String("Stream/Output")) ? Direction::Output : Direction::Input;
            st.nodeName = prop(node, PW_KEY_NODE_NAME);
            st.app.binary = prop(node, PW_KEY_APP_PROCESS_BINARY);
            st.app.name = prop(node, PW_KEY_APP_NAME);
            st.app.id = prop(node, PW_KEY_APP_ID);
            st.app.portalAppId = prop(node, "pipewire.access.portal.app_id");
            st.app.iconName = prop(node, PW_KEY_APP_ICON_NAME);
            st.app.key = !st.app.binary.isEmpty() ? st.app.binary : !st.app.name.isEmpty() ? st.app.name : st.nodeName;
            st.mediaRole = prop(node, PW_KEY_MEDIA_ROLE);
            st.channelVolumes = channels;
            st.muted = muted;
            st.running = wp_node_get_state(WP_NODE(node), nullptr) == WP_NODE_STATE_RUNNING;
            st.monitor = flag(node, PW_KEY_STREAM_MONITOR);
            st.capturesOutput = flag(node, PW_KEY_STREAM_CAPTURE_SINK);
            st.internal = !prop(node, PW_KEY_NODE_LINK_GROUP).isEmpty() || flag(node, PW_KEY_NODE_VIRTUAL);
            s.streams.append(st);
        }
        g_value_unset(&value);
    }
    wp_iterator_unref(it);

    // What each stream is linked to, and where it was sent.
    for (Stream& st : s.streams) {
        const QList<quint32> peers = st.direction == Direction::Output ? sendsTo.values(st.id) : receivesFrom.values(st.id);
        for (quint32 peer : peers) {
            if (s.endpoint(peer)) {
                st.endpoint = peer;
            }
            if (st.direction == Direction::Input && nodeClasses.value(peer).startsWith(QLatin1String("Audio/Sink"))) {
                st.capturesOutput = true;
            }
        }
        if (!metadata) {
            continue;
        }
        QString target = QString::fromUtf8(wp_metadata_find(metadata, st.id, "target.object", nullptr));
        if (target.isEmpty()) {
            target = QString::fromUtf8(wp_metadata_find(metadata, st.id, "target.node", nullptr));
        }
        if (target.size() >= 2 && target.startsWith(u'"') && target.endsWith(u'"')) {
            target = target.mid(1, target.size() - 2);
        }
        if (target.isEmpty() || target == QLatin1String("-1")) {
            continue;
        }
        bool numeric = false;
        const quint64 number = target.toULongLong(&numeric);
        for (const Endpoint& e : std::as_const(s.endpoints)) {
            if (e.name == target || (numeric && (e.serial == number || e.id == number))) {
                st.target = e.name;
            }
        }
    }

    for (const Endpoint& e : std::as_const(s.endpoints)) {
        if (e.isDefault) {
            (e.direction == Direction::Output ? s.defaultOutput : s.defaultInput) = e.name;
        }
    }
    if (metadata) {
        g_object_unref(metadata);
    }

    const bool wasConnected = m_snapshot.connected;
    m_snapshot = std::move(s);
    emit changed();
    if (!wasConnected) {
        emit connectedChanged();
    }
}

bool Core::setDefault(Direction direction, const QString& name)
{
    const Endpoint* e = m_snapshot.endpoint(name);
    WpMetadata* metadata = e && e->direction == direction ? d->metadata() : nullptr;
    if (!metadata) {
        return false;
    }
    const QByteArray value = QJsonDocument(QJsonObject { { QStringLiteral("name"), name } }).toJson(QJsonDocument::Compact);
    wp_metadata_set(metadata, 0,
        direction == Direction::Output ? "default.configured.audio.sink" : "default.configured.audio.source",
        "Spa:String:JSON", value.constData());
    g_object_unref(metadata);
    return true;
}

namespace {

GVariant* channelVolumesVariant(const QList<double>& volumes)
{
    GVariantBuilder channels;
    g_variant_builder_init(&channels, G_VARIANT_TYPE_VARDICT);
    for (qsizetype i = 0; i < volumes.size(); i++) {
        GVariantBuilder channel;
        g_variant_builder_init(&channel, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&channel, "{sv}", "volume", g_variant_new_double(std::clamp(volumes[i], 0.0, 1.0)));
        g_variant_builder_add(&channels, "{sv}", QByteArray::number(i).constData(), g_variant_builder_end(&channel));
    }
    GVariantBuilder all;
    g_variant_builder_init(&all, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&all, "{sv}", "channelVolumes", g_variant_builder_end(&channels));
    return g_variant_builder_end(&all);
}

} // namespace

static bool setVolumeVariant(WpPlugin* mixer, quint32 node, GVariant* variant)
{
    g_variant_ref_sink(variant);
    gboolean ok = FALSE;
    if (mixer) {
        g_signal_emit_by_name(mixer, "set-volume", guint(node), variant, &ok);
    }
    g_variant_unref(variant);
    return ok;
}

bool Core::setChannelVolumes(quint32 node, const QList<double>& volumes)
{
    if (volumes.isEmpty() || (!m_snapshot.endpoint(node) && !m_snapshot.stream(node))) {
        return false;
    }
    return setVolumeVariant(d->mixer, node, channelVolumesVariant(volumes));
}

bool Core::setVolume(quint32 node, double volume)
{
    volume = std::clamp(volume, 0.0, 1.0);
    if (const Endpoint* e = m_snapshot.endpoint(node)) {
        return setChannelVolumes(node, scaledChannels(e->channelVolumes, volume, d->balances.value(e->name)));
    }
    if (const Stream* s = m_snapshot.stream(node)) {
        return setChannelVolumes(node, QList<double>(qMax<qsizetype>(1, s->channelVolumes.size()), volume));
    }
    return false;
}

bool Core::setMuted(quint32 node, bool muted)
{
    if (!m_snapshot.endpoint(node) && !m_snapshot.stream(node)) {
        return false;
    }
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "mute", g_variant_new_boolean(muted));
    return setVolumeVariant(d->mixer, node, g_variant_builder_end(&b));
}

bool Core::setTarget(quint32 stream, const QString& endpoint)
{
    const Stream* s = m_snapshot.stream(stream);
    const Endpoint* e = endpoint.isEmpty() ? nullptr : m_snapshot.endpoint(endpoint);
    if (!s || (!endpoint.isEmpty() && (!e || e->direction != s->direction))) {
        return false;
    }
    WpMetadata* metadata = d->metadata();
    if (!metadata) {
        return false;
    }
    // -1: the default one again, and WirePlumber forgets the app's choice
    // (what PipeWire's pulse server does for "move to @DEFAULT_SINK@").
    const QByteArray value = e ? QByteArray::number(e->serial) : QByteArrayLiteral("-1");
    wp_metadata_set(metadata, stream, "target.node", nullptr, nullptr);
    wp_metadata_set(metadata, stream, "target.object", "Spa:Id", value.constData());
    g_object_unref(metadata);
    return true;
}

} // namespace vela::audio
