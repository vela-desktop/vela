// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "backgroundeffects.h"

#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QRectF>
#include <QtDebug>
#include <QtGui/qguiapplication_platform.h>
// Qt private API (as in foreigntoplevels.cpp): a window's wl_surface.
#include <QtGui/qpa/qplatformwindow_p.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <wayland-client.h>

#include "ext-background-effect-v1-client-protocol.h"

struct EffectCallbacks {
    static void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
    {
        auto* self = static_cast<BackgroundEffects*>(data);
        if (!strcmp(interface, wl_compositor_interface.name)) {
            self->m_compositor = static_cast<wl_compositor*>(
                wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
        } else if (!strcmp(interface, ext_background_effect_manager_v1_interface.name)) {
            self->m_manager = static_cast<ext_background_effect_manager_v1*>(
                wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1));
            ext_background_effect_manager_v1_add_listener(self->m_manager, &managerListener, self);
        }
    }
    static void globalRemove(void*, wl_registry*, uint32_t) { }
    static constexpr wl_registry_listener registryListener { global, globalRemove };

    static void capabilities(void* data, ext_background_effect_manager_v1*, uint32_t flags)
    {
        auto* self = static_cast<BackgroundEffects*>(data);
        const bool blur = flags & EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR;
        qInfo("vela-shell: compositor blur %s", blur ? "available" : "missing");
        if (blur != self->m_blurAvailable) {
            self->m_blurAvailable = blur;
            // The regions asked for before the capability arrived.
            for (auto& entry : self->m_entries) {
                self->apply(entry);
            }
            emit self->blurAvailableChanged();
        }
    }
    static constexpr ext_background_effect_manager_v1_listener managerListener { capabilities };
};

BackgroundEffects::BackgroundEffects(QObject* parent)
    : QObject(parent)
{
    auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!wayland || !wayland->display()) {
        return;
    }
    m_registry = wl_display_get_registry(wayland->display());
    wl_registry_add_listener(m_registry, &EffectCallbacks::registryListener, this);
    // Global and capability arrive with Qt's events: regions asked for earlier
    // are applied then (see capabilities).
    wl_display_flush(wayland->display());
}

BackgroundEffects::~BackgroundEffects()
{
    for (auto& entry : m_entries) {
        drop(entry);
    }
}

// A rounded rectangle as rows of pixels: only those entirely inside.
static QRegion roundedRegion(const QRect& rect, int radius)
{
    radius = std::min({ radius, rect.width() / 2, rect.height() / 2 });
    if (radius <= 0) {
        return QRegion(rect);
    }
    QRegion region(rect.adjusted(0, radius, 0, -radius));
    for (int i = 0; i < radius; ++i) {
        const double dy = radius - i; // the row's narrowest point, at its top edge
        const int inset = static_cast<int>(std::ceil(radius - std::sqrt(radius * radius - dy * dy)));
        region += QRect(rect.x() + inset, rect.y() + i, rect.width() - 2 * inset, 1);
        region += QRect(rect.x() + inset, rect.bottom() - i, rect.width() - 2 * inset, 1);
    }
    return region;
}

void BackgroundEffects::setBlur(QWindow* window, const QVariantList& rects, qreal radius)
{
    if (!window) {
        return;
    }
    QRegion region;
    for (const QVariant& value : rects) {
        region += roundedRegion(value.toRectF().toAlignedRect(), radius > 0 ? qRound(radius) : 0);
    }
    Entry& entry = m_entries[window];
    entry.window = window;
    if (entry.region == region && entry.effect) {
        return;
    }
    entry.region = region;
    if (!entry.connected) {
        entry.connected = true;
        connect(window, &QObject::destroyed, this, [this, window] { m_entries.remove(window); });
        // The native interface exists only once the window really exists.
        connect(window, &QWindow::visibleChanged, this, [this, window](bool visible) {
            if (visible && m_entries.contains(window)) {
                follow(m_entries[window]);
                apply(m_entries[window]);
            }
        });
        // Asked for while the native window didn't exist yet (a panel just
        // made visible): it's followed from its birth. Without this, the first
        // opening had no blur.
        window->installEventFilter(this);
    }
    follow(entry);
    apply(entry);
}

bool BackgroundEffects::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::PlatformSurface
        && static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
        auto* window = qobject_cast<QWindow*>(watched);
        auto entry = m_entries.find(window);
        if (entry != m_entries.end()) {
            drop(*entry);
            // Native window destruction also disconnects its signals. The
            // next platform window needs its own surface lifetime hooks.
            entry->following = false;
        }
    }
    if (event->type() == QEvent::PlatformSurface
        && static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
        // Right after the event: the native window has just been born.
        QMetaObject::invokeMethod(this, [this, window = QPointer<QWindow>(qobject_cast<QWindow*>(watched))] {
            if (window && m_entries.contains(window)) {
                follow(m_entries[window]);
                apply(m_entries[window]);
            }
        }, Qt::QueuedConnection);
    }
    return QObject::eventFilter(watched, event);
}

void BackgroundEffects::follow(Entry& entry)
{
    // The wl_surface changes when the window hides and comes back: it's
    // followed.
    if (entry.following || !entry.window) {
        return;
    }
    auto* native = entry.window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!native) {
        return;
    }
    entry.following = true;
    QWindow* window = entry.window;
    connect(native, &QNativeInterface::Private::QWaylandWindow::surfaceCreated, this, [this, window] {
        if (m_entries.contains(window)) {
            apply(m_entries[window]);
        }
    });
    connect(native, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, this, [this, window] {
        if (m_entries.contains(window)) {
            drop(m_entries[window]);
        }
    });
}

void BackgroundEffects::drop(Entry& entry)
{
    if (entry.effect) {
        ext_background_effect_surface_v1_destroy(entry.effect);
        entry.effect = nullptr;
    }
}

void BackgroundEffects::apply(Entry& entry)
{
    if (!m_manager || !m_compositor || !m_blurAvailable || !entry.window) {
        return;
    }
    auto* native = entry.window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    wl_surface* surface = native ? native->surface() : nullptr;
    if (!surface) {
        return; // the window isn't visible yet: applied at surfaceCreated
    }
    if (!entry.effect) {
        entry.effect = ext_background_effect_manager_v1_get_background_effect(m_manager, surface);
    }
    if (entry.region.isEmpty()) {
        ext_background_effect_surface_v1_set_blur_region(entry.effect, nullptr);
    } else {
        wl_region* region = wl_compositor_create_region(m_compositor);
        for (const QRect& rect : entry.region) {
            wl_region_add(region, rect.x(), rect.y(), rect.width(), rect.height());
        }
        ext_background_effect_surface_v1_set_blur_region(entry.effect, region);
        wl_region_destroy(region);
    }
    // Double-buffered state: applies from the next commit, which we ask for at
    // once.
    entry.window->requestUpdate();
}
