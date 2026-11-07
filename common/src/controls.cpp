// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "controls.h"

#include "iconprovider.h"

#include <QCoreApplication>

namespace vela::controls {

Appearance* install(QQmlEngine& engine, Appearance::Mode mode)
{
    if (!StyleForeign::instance) {
        StyleForeign::instance = new Appearance(QCoreApplication::instance(), mode);
    }
    if (!EffectsForeign::instance) {
        EffectsForeign::instance = new BackgroundEffects(QCoreApplication::instance());
    }
    engine.addImageProvider(QStringLiteral("icon"), new IconProvider);
    return StyleForeign::instance;
}

} // namespace vela::controls

Appearance* StyleForeign::create(QQmlEngine*, QJSEngine* engine)
{
    if (!instance) {
        instance = new Appearance(QCoreApplication::instance());
    }
    // The singleton belongs to the app, not to the QML engine.
    engine->setObjectOwnership(instance, QJSEngine::CppOwnership);
    return instance;
}

BackgroundEffects* EffectsForeign::create(QQmlEngine*, QJSEngine* engine)
{
    if (!instance) {
        instance = new BackgroundEffects(QCoreApplication::instance());
    }
    engine->setObjectOwnership(instance, QJSEngine::CppOwnership);
    return instance;
}
