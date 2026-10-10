# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# GuiPrivate is needed for a single private API (the taskbar's wl_surface, see
# foreigntoplevels.cpp): after a Qt update the shell must be rebuilt.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS DBus Gui GuiPrivate Qml Quick QuickControls2 Svg WaylandClient)
find_package(LayerShellQt REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(vela-shell
    src/main.cpp
    src/accessibility.cpp
    src/accessibility.h
    src/appmodel.cpp
    src/appmodel.h
    src/desktopexec.h
    src/mimeapps.h
    src/clipboard.cpp
    src/clipboard.h
    src/snip.cpp
    src/snip.h
    src/config.cpp
    src/config.h
    src/filethumbnails.cpp
    src/filethumbnails.h
    src/fileactions.cpp
    src/fileactions.h
    src/fileproperties.cpp
    src/fileproperties.h
    src/desktopmodel.cpp
    src/desktopmodel.h
    src/foreigntoplevels.cpp
    src/foreigntoplevels.h
    src/iconprovider.h
    src/jumplists.cpp
    src/jumplists.h
    src/links.h
    src/mediakeys.cpp
    src/mediakeys.h
    src/mixer.cpp
    src/mixer.h
    src/asyncprocess.h
    src/network.cpp
    src/network.h
    src/notifications.cpp
    src/shellcontroller.cpp
    src/shellcontroller.h
    src/servicemenus.cpp
    src/servicemenus.h
    src/systemactions.cpp
    src/systemactions.h
    src/systemstatus.cpp
    src/systemstatus.h
    src/taskbarmodel.cpp
    src/tray.cpp
    src/taskbarmodel.h
    src/volumesteps.h
    src/wallpaperprovider.h
    src/screenwindows.cpp
    src/screenwindows.h
    src/windowcapture.cpp
    src/windowcapture.h
    src/workspaces.cpp
    src/workspaces.h)

# The default wallpaper, inside the executable:
# ":/vela/images/vela_splash_169.svg".
qt_add_resources(vela-shell "images"
    PREFIX "/vela"
    BASE ${PROJECT_SOURCE_DIR}
    FILES ${PROJECT_SOURCE_DIR}/images/vela_splash_169.svg)

# The list of open windows, published by the compositor.
qt_generate_wayland_protocol_client_sources(vela-shell
    FILES ${PROJECT_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml)

# Window previews (Alt+Tab): ext-* protocols, used through libwayland directly
# (see windowcapture.cpp).
find_package(PkgConfig REQUIRED)
pkg_check_modules(SHELL_WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.37)
pkg_get_variable(SHELL_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_check_modules(SHELL_WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
set(SHELL_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${SHELL_GENERATED_DIR})
foreach(name
        ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1
        ext-image-capture-source/ext-image-capture-source-v1
        ext-image-copy-capture/ext-image-copy-capture-v1
        ext-data-control/ext-data-control-v1)
    get_filename_component(base ${name} NAME)
    set(xml ${SHELL_WAYLAND_PROTOCOLS_DIR}/staging/${name}.xml)
    set(header ${SHELL_GENERATED_DIR}/${base}-client-protocol.h)
    set(code ${SHELL_GENERATED_DIR}/${base}-protocol.c)
    add_custom_command(
        OUTPUT ${header} ${code}
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${header}
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${code}
        DEPENDS ${xml}
        COMMENT "Generating the ${base} client")
    target_sources(vela-shell PRIVATE ${header} ${code})
endforeach()
target_include_directories(vela-shell PRIVATE ${SHELL_GENERATED_DIR})

set(VELA_QML_FILES
    qml/Taskbar.qml
    qml/TaskbarButton.qml
    qml/StartGlyph.qml
    qml/StartMenu.qml
    qml/AppTile.qml
    qml/Switcher.qml
    qml/Wallpaper.qml
    qml/NotificationPopups.qml
    qml/Menus.qml
    qml/ContextMenu.qml
    qml/MenuPanel.qml
    qml/MenuTextField.qml
    qml/RunDialog.qml
    qml/ConfirmDialog.qml
    qml/SourceChooser.qml
    qml/PropertiesDialog.qml
    qml/PanelShadow.qml
    qml/QuickSettings.qml
    qml/NotificationCenter.qml
    qml/TaskView.qml
    qml/DesktopOsd.qml
    qml/VolumeOsd.qml
    qml/SnapLayouts.qml
    qml/SnapAssist.qml
    qml/TaskbarPreview.qml
    qml/FileDialogs.qml
    qml/ClipboardPanel.qml
    qml/SnipOverlay.qml)

# The files are in qml/ for tidiness, but in the module they must be at the
# root: so they see each other without imports.
foreach(file IN LISTS VELA_QML_FILES)
    get_filename_component(name ${file} NAME)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()
set_source_files_properties(qml/Menus.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(vela-shell
    URI Vela.Shell
    VERSION 1.0
    QML_FILES ${VELA_QML_FILES})

target_compile_options(vela-shell PRIVATE -Wall -Wextra)
target_link_libraries(vela-shell PRIVATE
    Qt6::DBus
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Svg
    Qt6::WaylandClient
    PkgConfig::SHELL_WAYLAND_CLIENT
    Qt6::GuiPrivate # the taskbar's wl_surface (see foreigntoplevels.cpp)
    LayerShellQt::Interface
    vela-controls # Theme, Button, Style, Effects (common/)
    vela-controlsplugin
    vela-app) # the mouse's Back and Forward buttons (common/)

install(TARGETS vela-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The tests (ctest): the Exec key of .desktop files, default apps and the
# volume's steps.
if(VELA_BUILD_TOOLS)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    qt_add_executable(vela-desktopexec-test tests/desktopexec.cpp src/desktopexec.h)
    target_include_directories(vela-desktopexec-test PRIVATE src)
    target_compile_options(vela-desktopexec-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-desktopexec-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME shell-exec-desktop COMMAND vela-desktopexec-test)

    qt_add_executable(vela-mimeapps-test tests/mimeapps.cpp src/mimeapps.h)
    target_include_directories(vela-mimeapps-test PRIVATE src)
    target_compile_options(vela-mimeapps-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-mimeapps-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME shell-mimeapps COMMAND vela-mimeapps-test)

    qt_add_executable(vela-volumesteps-test tests/volumesteps.cpp src/volumesteps.h)
    target_include_directories(vela-volumesteps-test PRIVATE src)
    target_compile_options(vela-volumesteps-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-volumesteps-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME shell-volumesteps COMMAND vela-volumesteps-test)

    qt_add_executable(vela-service-models-test
        tests/service_models.cpp
        src/asyncprocess.h src/mixer.cpp src/mixer.h src/network.cpp src/network.h
        src/appmodel.cpp src/appmodel.h
        ${PROJECT_SOURCE_DIR}/settings/src/audio.cpp
        ${PROJECT_SOURCE_DIR}/settings/src/audio.h)
    target_include_directories(vela-service-models-test PRIVATE src ${PROJECT_SOURCE_DIR}/settings/src)
    target_compile_options(vela-service-models-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-service-models-test PRIVATE Qt6::Core Qt6::DBus Qt6::Test)
    add_test(NAME shell-service-models COMMAND vela-service-models-test)
    set_tests_properties(shell-service-models PROPERTIES TIMEOUT 90)
endif()

# Italian translation: strings in the code are in English, Italian lives
# in i18n/vela-shell_it.ts and ends up in the executable (:/i18n/vela-shell_it.qm, see
# shell/src/language.h). After changing texts:
#   cmake --build build --target update_translations
find_package(Qt6 6.7 REQUIRED COMPONENTS LinguistTools)
qt_add_translations(TARGETS vela-shell SOURCE_TARGETS vela-shell
    TS_FILES i18n/vela-shell_it.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_TARGET vela-shell_lupdate
    LRELEASE_TARGET vela-shell_lrelease
    LUPDATE_OPTIONS -source-language en -target-language it -no-obsolete -locations none)
