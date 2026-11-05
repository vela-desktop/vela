// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "renderingpolicy.h"

#include <QQuickWindow>

#include <mutex>

namespace {

constexpr auto aaVariable = "QSG_DISTANCEFIELD_ANTIALIASING";
std::once_flag textOnce;
std::once_flag antialiasingOnce;
bool injectedGrayscale = false;

} // namespace

namespace vela::rendering {

void configureTextRenderer()
{
    std::call_once(textOnce, [] {
        // Native bitmaps snap and stretch under transforms. Never switch text
        // renderers between idle and animation, which could also shift metrics.
        QQuickWindow::setTextRenderType(QQuickWindow::QtTextRendering);
    });
}

void configureGrayscaleAntialiasing()
{
    std::call_once(antialiasingOnce, [] {
        // Qt exposes SDF AA through the environment, not QQuickWindow.
        // Respect explicit overrides, including an empty value.
        if (!qEnvironmentVariableIsSet(aaVariable)) {
            injectedGrayscale = qputenv(aaVariable, "gray");
        }
    });
}

void configureApplication()
{
    configureTextRenderer();
    configureGrayscaleAntialiasing();
}

QProcessEnvironment externalProcessEnvironment()
{
    auto environment = QProcessEnvironment::systemEnvironment();
    if (injectedGrayscale && qgetenv(aaVariable) == "gray") {
        environment.remove(QString::fromLatin1(aaVariable));
    }
    return environment;
}

} // namespace vela::rendering
