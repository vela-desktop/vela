# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-polkit-prompt: the User Account Control dialog (Qt Quick,
# LayerShellQt, Vela.Controls). Built into the agent's directory.

# Also the modules Vela.Controls uses: they are needed here to link them.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS DBus Gui GuiPrivate Qml Quick QuickControls2 Svg)
find_package(LayerShellQt REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(vela-polkit-prompt main.cpp request.cpp request.h ../protocol.hpp)
set_target_properties(vela-polkit-prompt PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..)

set(PROMPT_QML_FILES qml/Dialog.qml qml/Veil.qml)
foreach(file IN LISTS PROMPT_QML_FILES)
    get_filename_component(name ${file} NAME)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()
qt_add_qml_module(vela-polkit-prompt
    URI Vela.Polkit
    VERSION 1.0
    QML_FILES ${PROMPT_QML_FILES})

target_compile_options(vela-polkit-prompt PRIVATE -Wall -Wextra)
target_link_libraries(vela-polkit-prompt PRIVATE vela-alloc
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    vela-controls
    vela-controlsplugin
    LayerShellQt::Interface)

install(TARGETS vela-polkit-prompt RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

# Italian translation: strings in the code are in English, Italian lives in
# i18n/vela-polkit-prompt_it.ts and ends up in the executable (see
# shell/src/language.h). After changing texts:
#   cmake --build build --target update_translations
find_package(Qt6 6.7 REQUIRED COMPONENTS LinguistTools)
qt_add_translations(TARGETS vela-polkit-prompt SOURCE_TARGETS vela-polkit-prompt
    TS_FILES ../i18n/vela-polkit-prompt_it.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_TARGET vela-polkit-prompt_lupdate
    LRELEASE_TARGET vela-polkit-prompt_lrelease
    LUPDATE_OPTIONS -source-language en -target-language it -no-obsolete -locations none)
