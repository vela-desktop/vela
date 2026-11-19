// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QRect>
#include <QString>
#include <QtWaylandClient/QWaylandClientExtensionTemplate>

#include "qwayland-vela-toplevel-identifier-v1.h"
#include "qwayland-wlr-foreign-toplevel-management-unstable-v1.h"

class QWindow;

// Open windows, as the compositor publishes them with the
// wlr-foreign-toplevel-management protocol: title, app_id, state, and the
// requests to activate, minimize or close them.

class ForeignToplevel : public QObject, public QtWayland::zwlr_foreign_toplevel_handle_v1 {
    Q_OBJECT

public:
    explicit ForeignToplevel(::zwlr_foreign_toplevel_handle_v1* handle, QObject* parent = nullptr);
    ~ForeignToplevel() override;

    // Applied state: everything changes together at the "done" event.
    QString title;
    QString appId;
    QString identifier;
    bool activated = false;
    bool minimized = false;
    bool maximized = false;
    bool fullscreen = false;
    ForeignToplevel* parentWindow = nullptr; // such as a dialog
    quint64 lastActivated = 0; // to know which of an app's windows is the most recent
    bool ready = false; // received the first "done"

    void requestActivate();
    void requestMinimize();
    // Where its button is (in `panel`'s coordinates): the compositor flies the
    // window there when it's minimized.
    void setButtonRect(QWindow* panel, const QRect& rect);

signals:
    void changed();
    void closed();

protected:
    void zwlr_foreign_toplevel_handle_v1_title(const QString& title) override;
    void zwlr_foreign_toplevel_handle_v1_app_id(const QString& appId) override;
    void zwlr_foreign_toplevel_handle_v1_state(wl_array* state) override;
    void zwlr_foreign_toplevel_handle_v1_parent(::zwlr_foreign_toplevel_handle_v1* parent) override;
    void zwlr_foreign_toplevel_handle_v1_done() override;
    void zwlr_foreign_toplevel_handle_v1_closed() override;

private:
    struct {
        QString title;
        QString appId;
        bool activated = false;
        bool minimized = false;
        bool maximized = false;
        bool fullscreen = false;
        ForeignToplevel* parentWindow = nullptr;
    } m_pending;
    QHash<QWindow*, QRect> m_buttonRects;
};

class ToplevelIdentifier : public QWaylandClientExtensionTemplate<ToplevelIdentifier>,
                           public QtWayland::vela_toplevel_identifier_v1 {
    Q_OBJECT

public:
    ToplevelIdentifier();
    ~ToplevelIdentifier() override;

signals:
    void identified(uint32_t handleId, const QString& identifier);

protected:
    void vela_toplevel_identifier_v1_identified(uint32_t handleId, const QString& identifier) override;
};

class ForeignToplevelManager : public QWaylandClientExtensionTemplate<ForeignToplevelManager>,
                               public QtWayland::zwlr_foreign_toplevel_manager_v1 {
    Q_OBJECT

public:
    ForeignToplevelManager();

    // Only windows fully described, in opening order.
    const QList<ForeignToplevel*>& windows() const { return m_windows; }

signals:
    void windowsChanged();

protected:
    void zwlr_foreign_toplevel_manager_v1_toplevel(::zwlr_foreign_toplevel_handle_v1* handle) override;

private:
    QList<ForeignToplevel*> m_windows;
    ToplevelIdentifier m_identifier;
    QHash<uint32_t, ForeignToplevel*> m_handles;
};
