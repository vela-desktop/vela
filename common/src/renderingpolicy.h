// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace vela::rendering {

// Configure before constructing Qt Quick text. Individual QML Text elements
// can override renderType for an explicitly documented special case.
void configureTextRenderer();

// Separate concern: grayscale SDF coverage for translucent Vela surfaces.
// An explicitly supplied Qt antialiasing environment choice is respected.
void configureGrayscaleAntialiasing();

// Explicit and idempotent: call before QGuiApplication / Qt Quick startup.
void configureApplication();

// Render Qt Quick through Vulkan, like the compositor. A Vela architecture
// choice, not a platform default. An explicit QSG_RHI_BACKEND is respected
// (debugging); launched apps do not inherit the default. Call before the
// first QQuickWindow exists.
void configureVulkan();

} // namespace vela::rendering
