# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# What every Vela app (shell, Settings, Files, lock screen) gets just by
# linking vela-app: the mouse's side buttons as the Back and Forward commands
# (navigationbuttons.cpp). An object library, so its startup function is
# always linked in. GuiPrivate for one QPA call (the same as the keyboard's
# Back key takes): after a Qt update the apps must be rebuilt.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Gui GuiPrivate)

add_library(vela-app OBJECT navigationbuttons.cpp)
target_compile_options(vela-app PRIVATE -Wall -Wextra)
target_link_libraries(vela-app PUBLIC Qt6::Gui PRIVATE Qt6::GuiPrivate)

# Vela.Controls (docs/polkit-agent.md §8): theme, controls and blur shared by
# Vela's Qt Quick interfaces (the shell, Settings, Files, the polkit dialog).
#
# From C++: link vela-controls and vela-controlsplugin,
# Q_IMPORT_QML_PLUGIN(Vela_ControlsPlugin) and, before loading QML,
# vela::controls::install(engine, mode).
find_package(Qt6 6.5 REQUIRED COMPONENTS DBus Gui GuiPrivate Qml Quick QuickControls2 Svg)

# Process-local environment defaults and external launches need only Qt Core.
add_library(vela-process STATIC src/processenvironment.cpp)
target_include_directories(vela-process PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(vela-process PRIVATE -Wall -Wextra)
target_link_libraries(vela-process PUBLIC Qt6::Core)

# Explicit Qt Quick text policy, independent from the Vela.Controls module.
add_library(vela-rendering STATIC src/renderingpolicy.cpp)
target_include_directories(vela-rendering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(vela-rendering PRIVATE -Wall -Wextra)
target_link_libraries(vela-rendering PUBLIC Qt6::Quick PRIVATE vela-process)

# vela-color: Material's HCT color space, tonal palettes, the Tonal Spot
# scheme and a wallpaper's source color, ported from Material Color
# Utilities (Apache-2.0, color/LICENSE). Plain C++, no Qt: the shell, the
# apps' Style and the lock screen compute the same palette.
add_library(vela-color STATIC
    color/hct.cpp
    color/hct.h
    color/palette.cpp
    color/palette.h
    color/scheme.cpp
    color/scheme.h
    color/quantize.cpp
    color/quantize.h)
target_include_directories(vela-color PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/color)
target_compile_options(vela-color PRIVATE -Wall -Wextra)
set_target_properties(vela-color PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CONTROLS_WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_get_variable(CONTROLS_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

set(SHELL_SRC ${PROJECT_SOURCE_DIR}/shell/src)

# The tokens a theme file may set (shell/src/themefile.h), with their type:
# every `readonly property <color|int|real|bool> <name>: ... themed("<token>"`
# of Theme.qml (the shell's and the apps' colors and radii), and every key the
# shell's surface styles read in Styles.qml (styles.size()/flag(): a number,
# Theme.themed() in a color, real or string property: a color, a number or a
# variant). Written at configure time, and
# again whenever either file changes, so a token added there is themable at
# once; a line in another shape stops the configuration instead of being
# left out.
set(THEME_QML ${CMAKE_CURRENT_SOURCE_DIR}/controls/Theme.qml)
set(STYLES_QML ${PROJECT_SOURCE_DIR}/shell/qml/Styles.qml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${THEME_QML} ${STYLES_QML})
set(theme_tokens "")
set(theme_token_names "")
function(vela_theme_token name kind)
    if(NOT name IN_LIST theme_token_names)
        set(theme_token_names ${theme_token_names} ${name} PARENT_SCOPE)
        set(theme_tokens "${theme_tokens}    { \"${name}\", TokenKind::${kind} },\n" PARENT_SCOPE)
    endif()
endfunction()
file(STRINGS ${THEME_QML} themed_lines REGEX "themed\\(\"")
foreach(line IN LISTS themed_lines)
    if(NOT line MATCHES "readonly property (color|int|real|bool|string) [A-Za-z0-9_]+: .*themed\\(\"([A-Za-z0-9_-]+)\"")
        message(FATAL_ERROR "Theme.qml: themed() outside a `readonly property color|int|real|bool|string` line:\n${line}")
    endif()
    if(CMAKE_MATCH_1 STREQUAL "color")
        vela_theme_token(${CMAKE_MATCH_2} Color)
    elseif(CMAKE_MATCH_1 STREQUAL "string")
        vela_theme_token(${CMAKE_MATCH_2} Variant)
    else()
        vela_theme_token(${CMAKE_MATCH_2} Number)
    endif()
endforeach()
file(STRINGS ${STYLES_QML} styled_lines REGEX "themed\\(\"|styles\\.(size|flag)\\(\"")
foreach(line IN LISTS styled_lines)
    if(line MATCHES "styles\\.(size|flag)\\(\"([a-z0-9-]+)\"")
        vela_theme_token(${CMAKE_MATCH_2} Number)
    elseif(line MATCHES "readonly property color [A-Za-z0-9_]+: Theme\\.themed\\(\"([a-z0-9-]+)\"")
        vela_theme_token(${CMAKE_MATCH_1} Color)
    elseif(line MATCHES "readonly property real [A-Za-z0-9_]+: .*Theme\\.themed\\(\"([a-z0-9-]+)\"")
        vela_theme_token(${CMAKE_MATCH_1} Number)
    elseif(line MATCHES "readonly property string variant: Theme\\.themed\\(\"([a-z0-9-]+)\"")
        vela_theme_token(${CMAKE_MATCH_1} Variant)
    else()
        message(FATAL_ERROR "Styles.qml: a key read in a shape themetokens.h doesn't know:\n${line}")
    endif()
endforeach()
# The keys of the taskbar's layout: themefile.h's taskbarLayout() reads them,
# the one rule for Styles.qml, Start's placement and Settings.
vela_theme_token(taskbar-variant Variant)
vela_theme_token(taskbar-margin Number)
vela_theme_token(taskbar-radius Number)
vela_theme_token(taskbar-fit Number)
vela_theme_token(taskbar-edge Number)
if(theme_tokens STREQUAL "")
    message(FATAL_ERROR "Theme.qml: no themed() token found")
endif()
set(THEME_TOKENS_DIR ${CMAKE_CURRENT_BINARY_DIR}/themetokens)
file(CONFIGURE OUTPUT ${THEME_TOKENS_DIR}/themetokens.h CONTENT
"// Generated by common/CMakeLists.txt from common/controls/Theme.qml and
// shell/qml/Styles.qml: do not edit.
#pragma once

namespace vela::themes {

// Color: #RRGGBB or #AARRGGBB; Number; Variant: one of Vela's own layouts
// (themefile.h).
enum class TokenKind { Color, Number, Variant };

struct Token {
    const char* name;
    TokenKind kind;
};

inline constexpr Token tokens[] = {
@theme_tokens@};

} // namespace vela::themes
" @ONLY)
add_library(vela-theme-tokens INTERFACE)
target_include_directories(vela-theme-tokens INTERFACE ${THEME_TOKENS_DIR})

# The blur (ext-background-effect-v1), as in the shell.
set(CONTROLS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${CONTROLS_GENERATED_DIR})
set(xml ${CONTROLS_WAYLAND_PROTOCOLS_DIR}/staging/ext-background-effect/ext-background-effect-v1.xml)
add_custom_command(
    OUTPUT ${CONTROLS_GENERATED_DIR}/ext-background-effect-v1-client-protocol.h
           ${CONTROLS_GENERATED_DIR}/ext-background-effect-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${CONTROLS_GENERATED_DIR}/ext-background-effect-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${CONTROLS_GENERATED_DIR}/ext-background-effect-v1-protocol.c
    DEPENDS ${xml}
    COMMENT "Generating the ext-background-effect-v1 client")

qt_add_library(vela-controls STATIC)

set(CONTROLS_QML_FILES
    controls/Theme.qml
    controls/TonalSurface.qml
    controls/Fill.qml
    controls/Eased.qml
    controls/ColorFade.qml
    controls/Icon.qml
    controls/Chevron.qml
    controls/FocusRing.qml
    controls/Button.qml
    controls/Switch.qml
    controls/Slider.qml
    controls/TextBox.qml
    controls/ListItem.qml
    controls/Shadow.qml
    controls/PanelMotion.qml
    controls/Dialog.qml
    controls/StartIcon.qml)
foreach(file IN LISTS CONTROLS_QML_FILES)
    get_filename_component(name ${file} NAME)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()
set_source_files_properties(controls/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(vela-controls
    URI Vela.Controls
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Vela/Controls
    QML_FILES ${CONTROLS_QML_FILES}
    SOURCES
        src/controls.h
        src/controls.cpp
        ${SHELL_SRC}/appearance.cpp
        ${SHELL_SRC}/appearance.h
        ${SHELL_SRC}/backgroundeffects.cpp
        ${SHELL_SRC}/backgroundeffects.h
        ${SHELL_SRC}/iconprovider.h
        ${SHELL_SRC}/mica.h
        ${CONTROLS_GENERATED_DIR}/ext-background-effect-v1-client-protocol.h
        ${CONTROLS_GENERATED_DIR}/ext-background-effect-v1-protocol.c)

target_include_directories(vela-controls PUBLIC src ${SHELL_SRC} PRIVATE ${CONTROLS_GENERATED_DIR})
target_compile_options(vela-controls PRIVATE -Wall -Wextra)
target_link_libraries(vela-controls PUBLIC
    vela-color # Style.tonal (appearance.cpp)
    vela-rendering
    vela-process
    vela-theme-tokens
    Qt6::DBus
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
    PRIVATE
    Qt6::GuiPrivate # the windows' wl_surface (backgroundeffects.cpp)
    PkgConfig::CONTROLS_WAYLAND_CLIENT)

# The shared pieces under Qt Quick Test (tests/tst_*.qml): their states,
# fades, keyboard and accessibility, with animation effects on and off.
if(VELA_BUILD_TOOLS)
    find_package(Qt6 6.5 REQUIRED COMPONENTS QuickTest)
    qt_add_executable(vela-controls-test tests/controls_test.cpp)
    target_compile_definitions(vela-controls-test PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_options(vela-controls-test PRIVATE -Wall -Wextra)
    target_include_directories(vela-controls-test PRIVATE ${PROJECT_SOURCE_DIR}/common/tests)
    target_link_libraries(vela-controls-test PRIVATE vela-controls vela-controlsplugin Qt6::QuickTest)
    add_test(NAME controls COMMAND vela-controls-test)
    add_test(NAME controls-no-effects COMMAND vela-controls-test)
    set_tests_properties(controls-no-effects PROPERTIES ENVIRONMENT "VELA_TEST_ANIMATION_EFFECTS=no")
    set_tests_properties(controls controls-no-effects PROPERTIES TIMEOUT 120)

    # vela-color against Material Color Utilities' own results.
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    qt_add_executable(vela-color-test tests/color_test.cpp)
    target_compile_options(vela-color-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-color-test PRIVATE vela-color Qt6::Test)
    add_test(NAME color COMMAND vela-color-test)
    set_tests_properties(color PROPERTIES TIMEOUT 60)

    # Real-process environment tests need no GPU, display, or Qt 6.6 APIs.
    qt_add_executable(vela-process-launch-test tests/process_launch.cpp)
    target_compile_options(vela-process-launch-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-process-launch-test PRIVATE vela-process)
    add_test(NAME process-launch-environment COMMAND vela-process-launch-test)
    set_tests_properties(process-launch-environment PROPERTIES TIMEOUT 30)

    # Qt Quick glyph shader coverage uses OpenGL RHI (Mesa software under
    # Xvfb in hosted CI), not the compositor's real Vulkan GPU. Keep this
    # separate from real-GPU tests; never fall back to the software painter.
    if(Qt6_VERSION VERSION_GREATER_EQUAL 6.6)
        qt_add_executable(vela-text-rendering-test tests/text_rendering.cpp)
        target_compile_definitions(vela-text-rendering-test PRIVATE
            TEXT_SCENE="${CMAKE_CURRENT_SOURCE_DIR}/tests/TextRenderingScene.qml")
        target_compile_options(vela-text-rendering-test PRIVATE -Wall -Wextra)
        target_link_libraries(vela-text-rendering-test PRIVATE
            vela-controls vela-controlsplugin Qt6::GuiPrivate)
        add_test(NAME text-rendering-rhi COMMAND vela-text-rendering-test)
        set_tests_properties(text-rendering-rhi PROPERTIES
            TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS "rhi"
            ENVIRONMENT "QT_QPA_PLATFORMTHEME=generic")
    else()
        message(STATUS "Qt ${Qt6_VERSION}: RHI glyph test requires Qt >= 6.6")
    endif()
endif()

# vela-audio: the audio backend of the shell and Settings (docs/audio.md).
add_subdirectory(audio)
# vela-bluetooth-client: Bluetooth for the shell and Settings
# (docs/bluetooth.md §8).
add_subdirectory(bluetooth)
# vela-updates-client: updates for the shell and Settings
# (docs/updates.md §8).
add_subdirectory(updates)
