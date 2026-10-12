// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include "displays.h"

#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "vela-output-preview-v1-client-protocol.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QSocketNotifier>
#include <QTimer>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <vector>
#include <wayland-client.h>

namespace {
const QStringList transforms { QStringLiteral("normal"), QStringLiteral("90"), QStringLiteral("180"),
    QStringLiteral("270"), QStringLiteral("flipped"), QStringLiteral("flipped-90"),
    QStringLiteral("flipped-180"), QStringLiteral("flipped-270") };

template<typename T> void destroyProxy(T*& proxy)
{
    if (proxy) {
        wl_proxy_destroy(reinterpret_cast<wl_proxy*>(proxy));
        proxy = nullptr;
    }
}
}

struct Displays::Client {
    struct Head;
    struct Mode {
        Head* head;
        zwlr_output_mode_v1* proxy;
        int width = 0, height = 0, refresh = 0;
        bool preferred = false;
        ~Mode() { destroyProxy(proxy); }
        QVariantMap map(bool current) const
        {
            return { { "width", width }, { "height", height }, { "refresh", refresh / 1000.0 },
                { "preferred", preferred }, { "current", current } };
        }
    };
    struct Head {
        Client* client;
        zwlr_output_head_v1* proxy;
        QString name, description;
        bool enabled = false;
        int x = 0, y = 0, transform = 0;
        double scale = 1;
        Mode* current = nullptr;
        std::vector<std::unique_ptr<Mode>> modes;
        ~Head() { destroyProxy(proxy); }
    };

    Displays* owner;
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    zwlr_output_manager_v1* manager = nullptr;
    vela_output_preview_manager_v1* previewManager = nullptr;
    zwlr_output_configuration_v1* configuration = nullptr;
    vela_output_preview_v1* preview = nullptr;
    uint32_t managerName = 0, previewManagerName = 0, serial = 0;
    bool ready = false, confirming = false;
    int deadlineMs = 0;
    QElapsedTimer elapsed;
    QTimer countdown;
    std::unique_ptr<QSocketNotifier> read, write;
    std::vector<std::unique_ptr<Head>> heads;
    QVariantList published;
    QString error;

    explicit Client(Displays* owner, wl_display* connection) : owner(owner), display(connection)
    {
        countdown.setInterval(100);
        QObject::connect(&countdown, &QTimer::timeout, owner, [this] { emit this->owner->stateChanged(); });
        if (!display) return;
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener { global, globalRemove };
        wl_registry_add_listener(registry, &listener, this);
        read = std::make_unique<QSocketNotifier>(wl_display_get_fd(display), QSocketNotifier::Read);
        write = std::make_unique<QSocketNotifier>(wl_display_get_fd(display), QSocketNotifier::Write);
        write->setEnabled(false);
        QObject::connect(read.get(), &QSocketNotifier::activated, owner, [this] {
            // This connection belongs only to this GUI-thread client: a readable
            // fd can be dispatched without racing Qt's separate Wayland connection.
            if (wl_display_dispatch(display) < 0) disconnect();
            else flush();
        });
        QObject::connect(write.get(), &QSocketNotifier::activated, owner, [this] { flush(); });
        flush();
    }
    ~Client() { disconnect(false); }

    bool available() const { return display && ready && manager && previewManager; }
    void flush()
    {
        if (!display) return;
        if (wl_display_flush(display) < 0) {
            if (errno == EAGAIN) write->setEnabled(true);
            else {
                // Result callbacks can flush while Wayland is still dispatching.
                // Release the display only after that dispatch has returned.
                QTimer::singleShot(0, owner, [this] { disconnect(); });
            }
        } else write->setEnabled(false);
    }
    void disconnect(bool notify = true)
    {
        if (read) read->setEnabled(false);
        if (write) write->setEnabled(false);
        countdown.stop();
        destroyProxy(configuration);
        destroyProxy(preview);
        heads.clear();
        destroyProxy(manager);
        destroyProxy(previewManager);
        destroyProxy(registry);
        if (display) wl_display_disconnect(display);
        display = nullptr;
        ready = confirming = false;
        published.clear();
        if (notify) {
            error = Displays::tr("The display connection was lost. Reopen Settings to try again.");
            emit owner->outputsChanged();
            emit owner->stateChanged();
            emit owner->confirmationFinished();
        }
    }
    void clearRequest()
    {
        if (configuration) zwlr_output_configuration_v1_destroy(configuration);
        configuration = nullptr;
        if (preview) vela_output_preview_v1_destroy(preview);
        preview = nullptr;
        countdown.stop();
        confirming = false;
        emit owner->stateChanged();
        emit owner->confirmationFinished();
        flush();
    }
    static void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
    {
        auto* c = static_cast<Client*>(data);
        if (!std::strcmp(interface, zwlr_output_manager_v1_interface.name) && !c->manager) {
            c->managerName = name;
            c->manager = static_cast<zwlr_output_manager_v1*>(wl_registry_bind(registry, name,
                &zwlr_output_manager_v1_interface, std::min(version, 4u)));
            static const zwlr_output_manager_v1_listener listener { head, done, managerFinished };
            zwlr_output_manager_v1_add_listener(c->manager, &listener, c);
        } else if (!std::strcmp(interface, vela_output_preview_manager_v1_interface.name) && !c->previewManager) {
            c->previewManagerName = name;
            c->previewManager = static_cast<vela_output_preview_manager_v1*>(wl_registry_bind(registry, name,
                &vela_output_preview_manager_v1_interface, 1));
            emit c->owner->stateChanged();
        }
    }
    static void globalRemove(void* data, wl_registry*, uint32_t name)
    {
        auto* c = static_cast<Client*>(data);
        if (name == c->managerName || name == c->previewManagerName) {
            // Do not disconnect while dispatch is still using this display.
            QTimer::singleShot(0, c->owner, [c] { c->disconnect(); });
        }
    }
    static void head(void* data, zwlr_output_manager_v1*, zwlr_output_head_v1* proxy)
    {
        auto* c = static_cast<Client*>(data);
        auto h = std::make_unique<Head>();
        h->client = c;
        h->proxy = proxy;
        static const zwlr_output_head_v1_listener listener {
            .name = [](void* d, auto*, const char* v) { static_cast<Head*>(d)->name = QString::fromUtf8(v); },
            .description = [](void* d, auto*, const char* v) { static_cast<Head*>(d)->description = QString::fromUtf8(v); },
            .physical_size = [](void*, auto*, int32_t, int32_t) {},
            .mode = mode,
            .enabled = [](void* d, auto*, int32_t v) { static_cast<Head*>(d)->enabled = v; },
            .current_mode = [](void* d, auto*, zwlr_output_mode_v1* p) {
                auto* h = static_cast<Head*>(d);
                h->current = nullptr;
                for (auto& m : h->modes) if (m->proxy == p) h->current = m.get();
            },
            .position = [](void* d, auto*, int32_t x, int32_t y) { auto* h = static_cast<Head*>(d); h->x = x; h->y = y; },
            .transform = [](void* d, auto*, int32_t v) { static_cast<Head*>(d)->transform = v; },
            .scale = [](void* d, auto*, wl_fixed_t v) { static_cast<Head*>(d)->scale = wl_fixed_to_double(v); },
            .finished = [](void* d, zwlr_output_head_v1* p) {
                auto* h = static_cast<Head*>(d);
                auto* c = h->client;
                if (zwlr_output_head_v1_get_version(p) >= 3) { zwlr_output_head_v1_release(p); h->proxy = nullptr; }
                std::erase_if(c->heads, [h](const auto& item) { return item.get() == h; });
            },
            .make = [](void*, auto*, const char*) {},
            .model = [](void*, auto*, const char*) {},
            .serial_number = [](void*, auto*, const char*) {},
            .adaptive_sync = [](void*, auto*, uint32_t) {},
        };
        zwlr_output_head_v1_add_listener(proxy, &listener, h.get());
        c->heads.push_back(std::move(h));
    }
    static void mode(void* data, zwlr_output_head_v1*, zwlr_output_mode_v1* proxy)
    {
        auto* h = static_cast<Head*>(data);
        auto m = std::make_unique<Mode>();
        m->head = h;
        m->proxy = proxy;
        static const zwlr_output_mode_v1_listener listener {
            .size = [](void* d, auto*, int32_t w, int32_t h) { auto* m = static_cast<Mode*>(d); m->width = w; m->height = h; },
            .refresh = [](void* d, auto*, int32_t v) { static_cast<Mode*>(d)->refresh = v; },
            .preferred = [](void* d, auto*) { static_cast<Mode*>(d)->preferred = true; },
            .finished = [](void* d, zwlr_output_mode_v1* p) {
                auto* m = static_cast<Mode*>(d);
                auto* h = m->head;
                if (h->current == m) h->current = nullptr;
                if (zwlr_output_mode_v1_get_version(p) >= 3) { zwlr_output_mode_v1_release(p); m->proxy = nullptr; }
                std::erase_if(h->modes, [m](const auto& item) { return item.get() == m; });
            },
        };
        zwlr_output_mode_v1_add_listener(proxy, &listener, m.get());
        h->modes.push_back(std::move(m));
    }
    static void done(void* data, zwlr_output_manager_v1*, uint32_t serial)
    {
        auto* c = static_cast<Client*>(data);
        c->serial = serial;
        c->ready = true;
        QVariantList list;
        for (auto& h : c->heads) {
            QVariantList modes;
            for (auto& m : h->modes) modes.append(m->map(m.get() == h->current));
            QVariantMap output { { "name", h->name }, { "description", h->description }, { "enabled", h->enabled },
                { "x", h->x }, { "y", h->y }, { "scale", h->scale },
                { "transform", transforms.value(h->transform, transforms.first()) }, { "modes", modes } };
            if (h->current) output["current"] = h->current->map(true);
            list.append(output);
        }
        c->published = list;
        emit c->owner->outputsChanged();
        emit c->owner->stateChanged();
    }
    static void managerFinished(void* data, zwlr_output_manager_v1*)
    {
        auto* c = static_cast<Client*>(data);
        QTimer::singleShot(0, c->owner, [c] { c->disconnect(); });
    }
    static void previewApplied(void* data, vela_output_preview_v1*, uint32_t timeout)
    {
        auto* c = static_cast<Client*>(data);
        c->confirming = true;
        c->deadlineMs = static_cast<int>(timeout);
        c->elapsed.start();
        c->countdown.start();
        emit c->owner->stateChanged();
        emit c->owner->confirmationRequested();
    }
    static void previewFinished(void* data, vela_output_preview_v1*, uint32_t result)
    {
        auto* c = static_cast<Client*>(data);
        if (result == VELA_OUTPUT_PREVIEW_V1_RESULT_BUSY)
            c->error = Displays::tr("Another display change is awaiting confirmation.");
        else if (result == VELA_OUTPUT_PREVIEW_V1_RESULT_FAILED)
            c->error = Displays::tr("The display settings could not be applied. Try again.");
        else if (result == VELA_OUTPUT_PREVIEW_V1_RESULT_RESTORE_FAILED)
            c->error = Displays::tr("The previous display settings could not be fully restored.");
        c->clearRequest();
    }
    static void succeeded(void* data, zwlr_output_configuration_v1* configuration)
    {
        auto* c = static_cast<Client*>(data);
        zwlr_output_configuration_v1_destroy(configuration);
        c->configuration = nullptr;
    }
    static void failed(void* data, zwlr_output_configuration_v1*)
    {
        auto* c = static_cast<Client*>(data);
        c->error = Displays::tr("The display settings could not be applied. Try again.");
        c->clearRequest();
    }
    static void cancelled(void* data, zwlr_output_configuration_v1*)
    {
        auto* c = static_cast<Client*>(data);
        c->error = Displays::tr("The connected displays changed. Try again.");
        c->clearRequest();
    }
};

Displays::Displays(QObject* parent)
    : Displays(qGuiApp && QGuiApplication::platformName().startsWith(QLatin1String("wayland"))
            ? wl_display_connect(nullptr) : nullptr, parent) {}
Displays::Displays(wl_display* connection, QObject* parent)
    : QObject(parent), m_client(std::make_unique<Client>(this, connection)) {}
Displays::~Displays() = default;
bool Displays::available() const { return m_client->available(); }
bool Displays::pending() const { return m_client->preview || m_client->configuration; }
bool Displays::confirming() const { return m_client->confirming; }
int Displays::secondsRemaining() const
{
    return confirming() ? std::max(0, static_cast<int>((m_client->deadlineMs - m_client->elapsed.elapsed() + 999) / 1000)) : 0;
}
QString Displays::error() const { return m_client->error; }
QVariantList Displays::outputs() const { return m_client->published; }
void Displays::refresh() { m_client->flush(); }

bool Displays::apply(const QString& name, const QVariantMap& changes)
{
    auto& c = *m_client;
    if (!available() || pending()) return false;
    Client::Head* target = nullptr;
    for (auto& h : c.heads) if (h->name == name) target = h.get();
    if (!target) return false;
    const bool enabled = changes.value("enabled", true).toBool();
    const double scale = changes.value("scale", target->scale).toDouble();
    const int transform = transforms.indexOf(changes.value("transform", transforms.value(target->transform)).toString());
    if (!std::isfinite(scale) || scale <= 0 || scale > 16 || transform < 0) return false;
    Client::Mode* selected = nullptr;
    if (changes.contains("width") || changes.contains("height") || changes.contains("refresh")) {
        const int width = changes.value("width", target->current ? target->current->width : 0).toInt();
        const int height = changes.value("height", target->current ? target->current->height : 0).toInt();
        const double hz = changes.value("refresh", 0).toDouble();
        if (width <= 0 || height <= 0 || !std::isfinite(hz) || hz < 0 || hz > 10000) return false;
        const int refresh = static_cast<int>(std::lround(hz * 1000));
        for (auto& m : target->modes) {
            if (m->width == width && m->height == height && (!refresh || std::abs(m->refresh - refresh) <= 1)
                && (!selected || m->refresh > selected->refresh)) selected = m.get();
        }
        // Select advertised modes, including wlroots' virtual custom modes.
        if (!selected) return false;
    }
    if (!enabled && target->enabled && std::count_if(c.heads.begin(), c.heads.end(),
            [](const auto& h) { return h->enabled; }) <= 1) return false;
    c.error.clear();
    c.configuration = zwlr_output_manager_v1_create_configuration(c.manager, c.serial);
    static const zwlr_output_configuration_v1_listener configurationListener {
        Client::succeeded, Client::failed, Client::cancelled };
    zwlr_output_configuration_v1_add_listener(c.configuration, &configurationListener, &c);
    for (auto& h : c.heads) {
        const bool mine = h.get() == target;
        if (!(mine ? enabled : h->enabled)) {
            zwlr_output_configuration_v1_disable_head(c.configuration, h->proxy);
            continue;
        }
        auto* wanted = zwlr_output_configuration_v1_enable_head(c.configuration, h->proxy);
        if (mine) {
            if (selected) zwlr_output_configuration_head_v1_set_mode(wanted, selected->proxy);
            // Disabled heads do not advertise scale/position/transform. Leave
            // omitted properties to the compositor, which remembers them.
            if (changes.contains("scale"))
                zwlr_output_configuration_head_v1_set_scale(wanted, wl_fixed_from_double(scale));
            if (changes.contains("transform"))
                zwlr_output_configuration_head_v1_set_transform(wanted, transform);
            if (changes.contains("x") || changes.contains("y"))
                zwlr_output_configuration_head_v1_set_position(wanted,
                    changes.value("x", h->x).toInt(), changes.value("y", h->y).toInt());
        }
        destroyProxy(wanted);
    }
    c.preview = vela_output_preview_manager_v1_protect(c.previewManager,
        wl_proxy_get_id(reinterpret_cast<wl_proxy*>(c.configuration)),
        !target->enabled && !changes.contains("x") && !changes.contains("y") ? name.toUtf8().constData() : "");
    static const vela_output_preview_v1_listener previewListener { Client::previewApplied, Client::previewFinished };
    vela_output_preview_v1_add_listener(c.preview, &previewListener, &c);
    zwlr_output_configuration_v1_apply(c.configuration);
    emit stateChanged();
    c.flush();
    return pending();
}

void Displays::confirm()
{
    if (confirming() && m_client->preview) {
        vela_output_preview_v1_confirm(m_client->preview);
        m_client->flush();
    }
}
void Displays::revert()
{
    if (m_client->preview) {
        vela_output_preview_v1_revert(m_client->preview);
        m_client->flush();
    }
}
