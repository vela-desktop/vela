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
find_package(PkgConfig REQUIRED)
pkg_check_modules(CONTROLS_WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_get_variable(CONTROLS_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

set(SHELL_SRC ${PROJECT_SOURCE_DIR}/shell/src)

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
    controls/Dialog.qml)
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
    vela-rendering
    vela-process
