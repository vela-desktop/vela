// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QProcessEnvironment>

namespace vela::rendering {

// Configure before constructing Qt Quick text. Individual QML Text elements
// can override renderType for an explicitly documented special case.
void configureTextRenderer();

// Separate concern: grayscale SDF coverage for translucent Vela surfaces.
// An explicitly supplied Qt antialiasing environment choice is respected.
void configureGrayscaleAntialiasing();

// Explicit and idempotent: call before QGuiApplication / Qt Quick startup.
void configureApplication();

// Remove only the antialiasing setting Vela injected. Preserve the user's
// original environment and all unrelated variables for launched programs.
QProcessEnvironment externalProcessEnvironment();

} // namespace vela::rendering
