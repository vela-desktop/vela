// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "foreigntoplevels.h"

#include <QGuiApplication>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>
// Qt private API (KDE uses it too): the only way to get a window's wl_surface.
// It can change between minor Qt versions.
#include <QtGui/qpa/qplatformwindow_p.h>
#include <wayland-client-core.h>

namespace {

quint64 activationCounter = 0;

} // namespace

// ------------------------------------------------------- ForeignToplevel --

ForeignToplevel::ForeignToplevel(::zwlr_foreign_toplevel_handle_v1* handle, QObject* parent)
    : QObject(parent)
{
    if (handle) {
        init(handle);
    }
}

ForeignToplevel::~ForeignToplevel()
{
    if (isInitialized()) {
        destroy();
    }
}

void ForeignToplevel::requestActivate()
{
    auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (wayland && wayland->seat() && isInitialized()) {
        activate(wayland->seat());
    }
}

void ForeignToplevel::requestMinimize()
{
    if (isInitialized()) {
        set_minimized();
    }
}

void ForeignToplevel::requestMaximize(bool on)
{
    if (isInitialized()) {
        on ? set_maximized() : unset_maximized();
    }
}

void ForeignToplevel::requestClose()
{
    if (isInitialized()) {
        close();
    }
}

void ForeignToplevel::setButtonRect(QWindow* panel, const QRect& rect)
{
    if (!panel || (m_buttonRects.contains(panel) && m_buttonRects.value(panel) == rect)) {
        return;
    }
    auto* wayland = panel->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!wayland || !wayland->surface() || !isInitialized()) {
        return;
    }
    if (!m_buttonRects.contains(panel)) {
        connect(panel, &QObject::destroyed, this, [this, panel] { m_buttonRects.remove(panel); });
    }
    m_buttonRects.insert(panel, rect);
    set_rectangle(wayland->surface(), rect.x(), rect.y(), rect.width(), rect.height());
}

void ForeignToplevel::zwlr_foreign_toplevel_handle_v1_title(const QString& value)
{
    m_pending.title = value;
}

void ForeignToplevel::zwlr_foreign_toplevel_handle_v1_app_id(const QString& value)
{
    m_pending.appId = value;
}

void ForeignToplevel::zwlr_foreign_toplevel_handle_v1_state(wl_array* state)
{
    m_pending.activated = false;
    m_pending.minimized = false;
    m_pending.maximized = false;
    m_pending.fullscreen = false;
    const auto* values = static_cast<const uint32_t*>(state->data);
    for (size_t i = 0; i < state->size / sizeof(uint32_t); ++i) {
        switch (values[i]) {
        case state_activated: m_pending.activated = true; break;
        case state_minimized: m_pending.minimized = true; break;
        case state_maximized: m_pending.maximized = true; break;
        case state_fullscreen: m_pending.fullscreen = true; break;
        default: break;
        }
    }
}

void ForeignToplevel::zwlr_foreign_toplevel_handle_v1_parent(::zwlr_foreign_toplevel_handle_v1* parent)
{
    m_pending.parentWindow = parent ? static_cast<ForeignToplevel*>(fromObject(parent)) : nullptr;
}

void ForeignToplevel::zwlr_foreign_toplevel_handle_v1_done()
{
    if (m_pending.activated && !activated) {
        lastActivated = ++activationCounter;
    }
    title = m_pending.title;
    appId = m_pending.appId;
    activated = m_pending.activated;
    minimized = m_pending.minimized;
    maximized = m_pending.maximized;
    fullscreen = m_pending.fullscreen;
    parentWindow = m_pending.parentWindow;
    ready = true;
    emit changed();
}

void ForeignToplevel::zwlr_foreign_toplevel_handle_v1_closed()
{
    emit closed();
}

ToplevelIdentifier::ToplevelIdentifier()
    : QWaylandClientExtensionTemplate(1)
{
    initialize();
}

ToplevelIdentifier::~ToplevelIdentifier()
{
    if (isInitialized()) {
        destroy();
    }
}

void ToplevelIdentifier::vela_toplevel_identifier_v1_identified(uint32_t handleId, const QString& identifier)
{
    emit identified(handleId, identifier);
}

// ------------------------------------------------ ForeignToplevelManager --

ForeignToplevelManager::ForeignToplevelManager()
    : QWaylandClientExtensionTemplate(3)
{
    connect(&m_identifier, &ToplevelIdentifier::identified, this, [this](uint32_t handleId, const QString& identifier) {
        if (auto* window = m_handles.value(handleId); window && !identifier.isEmpty()) {
            window->identifier = identifier;
            emit windowsChanged();
        }
    });
    connect(&m_identifier, &ToplevelIdentifier::activeChanged, this, [this] {
        if (m_identifier.isActive()) {
            for (auto it = m_handles.cbegin(); it != m_handles.cend(); ++it) {
                m_identifier.identify(it.key());
            }
        }
    });
    initialize();
}

void ForeignToplevelManager::zwlr_foreign_toplevel_manager_v1_toplevel(::zwlr_foreign_toplevel_handle_v1* handle)
{
    adopt(new ForeignToplevel(handle, this), wl_proxy_get_id(reinterpret_cast<wl_proxy*>(handle)));
}

void ForeignToplevelManager::adopt(ForeignToplevel* window, uint32_t handleId)
{
    window->setParent(this);
    m_handles.insert(handleId, window);
    if (m_identifier.isActive()) {
        m_identifier.identify(handleId);
    }

    connect(window, &ForeignToplevel::changed, this, [this, window] {
        if (!m_windows.contains(window)) {
            m_windows.append(window); // at the first "done": now we know who it is
        }
        emit windowsChanged();
    });
    connect(window, &ForeignToplevel::closed, this, [this, window, handleId] {
        m_handles.remove(handleId);
        // Whoever points at this window as parent must not be left dangling.
        for (ForeignToplevel* other : std::as_const(m_windows)) {
            if (other->parentWindow == window) {
                other->parentWindow = nullptr;
            }
        }
        const bool wasListed = m_windows.removeOne(window);
        window->deleteLater();
        if (wasListed) {
            emit windowsChanged();
        }
    });
}
