// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "controls.h"

#include "iconprovider.h"

#include <QCoreApplication>
#include <QQuickWindow>

namespace {

void configureTextRendering()
{
    // Keep one scalable layout during motion and at rest. Native glyph
    // bitmaps snap and stretch under transforms; switching renderers during
    // motion would also change font metrics and make labels jump.
    QQuickWindow::setTextRenderType(QQuickWindow::QtTextRendering);
    // RGB antialiasing assumes a particular display's subpixel order. Our
    // translucent surfaces can move between outputs and over any background.
    // Gray coverage composes correctly there, and respects an explicit override.
    if (qEnvironmentVariableIsEmpty("QSG_DISTANCEFIELD_ANTIALIASING")) {
        qputenv("QSG_DISTANCEFIELD_ANTIALIASING", "gray");
    }
}

} // namespace

// Before any QQuickWindow creates its scene graph, including Quick Test's
// window, which exists before Setup::qmlEngineAvailable().
Q_COREAPP_STARTUP_FUNCTION(configureTextRendering)

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
