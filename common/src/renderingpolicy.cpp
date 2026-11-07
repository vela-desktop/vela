// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "renderingpolicy.h"
#include "processenvironment.h"

#include <QQuickWindow>

#include <mutex>

namespace {

std::once_flag textOnce;
std::once_flag antialiasingOnce;

} // namespace

namespace vela::rendering {

/** Select stable scalable glyph rendering before any Qt Quick text is constructed. */
void configureTextRenderer()
{
    std::call_once(textOnce, [] {
        // Native bitmaps snap and stretch under transforms. Never switch text
        // renderers between idle and animation, which could also shift metrics.
        QQuickWindow::setTextRenderType(QQuickWindow::QtTextRendering);
    });
}

/** Request neutral distance-field coverage without overriding the user's AA choice. */
void configureGrayscaleAntialiasing()
{
    std::call_once(antialiasingOnce, [] {
        // Qt exposes SDF AA through the environment, not QQuickWindow.
        // Respect explicit overrides, including an empty value.
        process::setPrivateEnvironmentDefault("QSG_DISTANCEFIELD_ANTIALIASING", "gray");
    });
}

/** Configure the two text policies once, before QGuiApplication starts its scene graph. */
void configureApplication()
{
    configureTextRenderer();
    configureGrayscaleAntialiasing();
}

} // namespace vela::rendering
