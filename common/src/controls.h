// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Vela.Controls' C++ singletons: the look (Style: light or dark, accent, Mica,
// icons) and the compositor's blur (Effects). The app creates them with
// install(), before loading QML.

#include "appearance.h"
#include "backgroundeffects.h"

#include <QQmlEngine>

namespace vela::controls {

// Mode::Shell for system UI (the shell, the polkit dialog), Mode::Apps for
// apps. Also registers the icons (image://icon/...). Returns Style: call its
// watch() to follow Settings live (after the first frame, if startup counts).
// Rendering is configured explicitly by each Vela UI app, not by linking
// the controls library.
Appearance* install(QQmlEngine& engine, Appearance::Mode mode);

} // namespace vela::controls

struct StyleForeign {
    Q_GADGET
    QML_FOREIGN(Appearance)
    QML_NAMED_ELEMENT(Style)
    QML_SINGLETON
public:
    inline static Appearance* instance = nullptr;
    static Appearance* create(QQmlEngine*, QJSEngine* engine);
};

struct EffectsForeign {
    Q_GADGET
    QML_FOREIGN(BackgroundEffects)
    QML_NAMED_ELEMENT(Effects)
    QML_SINGLETON
public:
    inline static BackgroundEffects* instance = nullptr;
    static BackgroundEffects* create(QQmlEngine*, QJSEngine* engine);
};
