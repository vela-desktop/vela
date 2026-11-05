// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRegion>
#include <QVariantList>
#include <QWindow>

struct wl_registry;
struct wl_compositor;
struct ext_background_effect_manager_v1;
struct ext_background_effect_surface_v1;

// The blur behind the shell's panels (taskbar, Start menu, menus,
// notifications, Alt+Tab), asked of the compositor with the standard
// ext-background-effect-v1 protocol (docs/renderer.md §8.3). The compositor
// blurs what's behind the region and clips it with the panel's alpha: rounded
// corners stay rounded.
//
// From QML: Effects.setBlur(window, [Qt.rect(...), ...]) whenever the area
// changes; Effects.blurAvailable tells whether the compositor can do it (if
// not, panels go back to opaque).
class BackgroundEffects : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool blurAvailable READ blurAvailable NOTIFY blurAvailableChanged)

public:
    explicit BackgroundEffects(QObject* parent = nullptr);
    ~BackgroundEffects() override;

    bool blurAvailable() const { return m_blurAvailable; }
    // Rectangles in window coordinates; empty list: no blur. radius: their
    // corners are rounded (a floating taskbar's pills): the pixels the curve
    // crosses stay unblurred, so there the panel's own antialiased edge meets
    // the background.
    Q_INVOKABLE void setBlur(QWindow* window, const QVariantList& rects, qreal radius = 0);

signals:
    void blurAvailableChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend struct EffectCallbacks;
    struct Entry {
        QPointer<QWindow> window;
        QRegion region;
        ext_background_effect_surface_v1* effect = nullptr;
        bool connected = false;
        bool following = false; // connected to the wl_surface signals
    };
    void follow(Entry& entry);
    void apply(Entry& entry);
    void drop(Entry& entry);

    wl_registry* m_registry = nullptr;
    wl_compositor* m_compositor = nullptr;
    ext_background_effect_manager_v1* m_manager = nullptr;
    bool m_blurAvailable = false;
    QHash<QWindow*, Entry> m_entries;
};
