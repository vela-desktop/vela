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
    src/audioapps.h
    src/desktopexec.h
    src/mimeapps.h
    src/clipboard.cpp
    src/clipboard.h
    src/snip.cpp
    src/snip.h
    src/snipextras.h
    src/soundkeys.cpp
    src/soundkeys.h
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
    src/asyncprocess.h
    src/bluetoothprompts.cpp
    src/bluetoothprompts.h
    src/network.cpp
    src/network.h
    src/notifications.cpp
    src/shellcontroller.cpp
    src/shellcontroller.h
    src/servicemenus.cpp
    src/servicemenus.h
    src/systemactions.cpp
    src/systemactions.h
    src/velatool.h
    src/systemstatus.cpp
    src/systemstatus.h
    src/brightness.cpp
    src/brightness.h
    src/transfermonitor.cpp
    src/transfermonitor.h
    src/taskbarmodel.cpp
    src/tray.cpp
    src/taskbarmodel.h
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
    qml/tiles.js
    qml/AppTile.qml
    qml/Switcher.qml
    qml/Wallpaper.qml
    qml/NotificationPopups.qml
    qml/Menus.qml
    qml/ContextMenu.qml
    qml/MenuPanel.qml
    qml/MenuTextField.qml
    qml/MenuTextBox.qml
    qml/TextMenu.qml
    qml/RunDialog.qml
    qml/ConfirmDialog.qml
    qml/BluetoothDialog.qml
    qml/BluetoothDevices.qml
    qml/SourceChooser.qml
    qml/PropertiesDialog.qml
    qml/CheckBox.qml
    qml/QuickSettings.qml
    qml/NotificationCenter.qml
    qml/TaskView.qml
    qml/DesktopOsd.qml
    qml/VolumeOsd.qml
    qml/SnapLayouts.qml
    qml/SnapAssist.qml
    qml/TaskbarPreview.qml
    qml/TaskbarTooltip.qml
    qml/FileDialogs.qml
    qml/ClipboardPanel.qml
    qml/SnipOverlay.qml
    qml/SnipCountdown.qml
    qml/SnipRecordBar.qml
    qml/ShadowRoom.qml)

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
    vela-capture
    vela-app # the mouse's Back and Forward buttons (common/)
    vela-audio # the sound (common/audio, docs/audio.md)
    vela-bluetooth-client # Bluetooth (common/bluetooth, docs/bluetooth.md)
    vela-bluetooth-uiplugin)

install(TARGETS vela-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS ../scripts/vela-ocr DESTINATION ${CMAKE_INSTALL_BINDIR})

# The tests (ctest): the Exec key of .desktop files, default apps, the
# volume's steps and the animation effects switch Style reads.
if(VELA_BUILD_TOOLS)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    qt_add_executable(vela-transfer-monitor-test
        tests/transfers.cpp src/transfermonitor.cpp src/transfermonitor.h)
    target_include_directories(vela-transfer-monitor-test PRIVATE src)
    target_compile_options(vela-transfer-monitor-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-transfer-monitor-test PRIVATE Qt6::Core Qt6::DBus Qt6::Test)
    find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
    if(DBUS_RUN_SESSION_EXECUTABLE)
        add_test(NAME shell-transfers
            COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:vela-transfer-monitor-test>)
    else()
        add_test(NAME shell-transfers COMMAND vela-transfer-monitor-test)
    endif()

    # Run the same ELF/AppImage classification used by desktop and Vela Files.
    qt_add_executable(vela-executable-file-test tests/executablefile.cpp ${PROJECT_SOURCE_DIR}/common/executablefile.h)
    target_compile_options(vela-executable-file-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-executable-file-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME shell-executable-file COMMAND vela-executable-file-test)

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

    # The shell's own pieces (tests/tst_*.qml) under Qt Quick Test, with fakes
    # of what its windows find in their context (Vela.Bluetooth, Shell,
    # BluetoothPrompts), with animation effects on and off.
    find_package(Qt6 6.5 REQUIRED COMPONENTS QuickTest)
    qt_add_executable(vela-shell-test tests/shell_test.cpp)
    target_compile_definitions(vela-shell-test PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
        FAKE_BLUETOOTH="${PROJECT_SOURCE_DIR}/common/bluetooth/tests/FakeBluetooth.qml")
    target_compile_options(vela-shell-test PRIVATE -Wall -Wextra)
    target_include_directories(vela-shell-test PRIVATE ${PROJECT_SOURCE_DIR}/common/tests)
    target_link_libraries(vela-shell-test PRIVATE vela-controls vela-controlsplugin vela-bluetooth-uiplugin Qt6::QuickTest)
    add_test(NAME shell-controls COMMAND vela-shell-test)
    add_test(NAME shell-controls-no-effects COMMAND vela-shell-test)
    set_tests_properties(shell-controls-no-effects PROPERTIES ENVIRONMENT "VELA_TEST_ANIMATION_EFFECTS=no")
    set_tests_properties(shell-controls shell-controls-no-effects PROPERTIES TIMEOUT 60)

    qt_add_executable(vela-tool-path-test tests/velatool.cpp src/velatool.h)
    target_include_directories(vela-tool-path-test PRIVATE src)
    target_compile_options(vela-tool-path-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-tool-path-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME shell-tool-path COMMAND vela-tool-path-test)

    qt_add_executable(vela-appmodel-watch-test
        tests/appmodel_watch.cpp src/appmodel.cpp src/appmodel.h src/desktopexec.h src/mimeapps.h)
    target_include_directories(vela-appmodel-watch-test PRIVATE src)
    target_compile_options(vela-appmodel-watch-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-appmodel-watch-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME shell-appmodel-watch COMMAND vela-appmodel-watch-test)

    qt_add_executable(vela-private-files-test tests/privatefiles.cpp)
    target_compile_options(vela-private-files-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-private-files-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME shell-private-files COMMAND vela-private-files-test)

    qt_add_executable(vela-snipcrop-test tests/snipcrop.cpp src/snipcrop.h)
    target_include_directories(vela-snipcrop-test PRIVATE src)
    target_compile_options(vela-snipcrop-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-snipcrop-test PRIVATE Qt6::Gui Qt6::Test)
    add_test(NAME shell-snipcrop COMMAND vela-snipcrop-test)

    qt_add_executable(vela-snipextras-test tests/snipextras.cpp src/snipextras.h)
    target_include_directories(vela-snipextras-test PRIVATE src)
    target_compile_options(vela-snipextras-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-snipextras-test PRIVATE Qt6::Gui Qt6::Test)
    add_test(NAME shell-snipextras COMMAND vela-snipextras-test)

    qt_add_executable(vela-volumesteps-test tests/volumesteps.cpp ${PROJECT_SOURCE_DIR}/common/audio/volumesteps.h)
    target_include_directories(vela-volumesteps-test PRIVATE ${PROJECT_SOURCE_DIR}/common/audio)
    target_compile_options(vela-volumesteps-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-volumesteps-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME shell-volumesteps COMMAND vela-volumesteps-test)

    qt_add_executable(vela-service-models-test
        tests/service_models.cpp
        src/asyncprocess.h src/network.cpp src/network.h)
    target_include_directories(vela-service-models-test PRIVATE src)
    target_compile_options(vela-service-models-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-service-models-test PRIVATE Qt6::Core Qt6::DBus Qt6::Test)
    add_test(NAME shell-service-models COMMAND vela-service-models-test)
    set_tests_properties(shell-service-models PROPERTIES TIMEOUT 90)

    # Switches backed by NetworkManager move at once and come back if the
    # service fails (src/requested.h), against a fake on a private bus.
    qt_add_executable(vela-requested-switches-test
        tests/requested_switches.cpp
        src/requested.h src/systemstatus.cpp src/systemstatus.h)
    target_include_directories(vela-requested-switches-test PRIVATE src)
    target_compile_options(vela-requested-switches-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-requested-switches-test PRIVATE vela-bluetooth-client Qt6::Core Qt6::DBus Qt6::Test)
    add_test(NAME shell-requested-switches COMMAND vela-requested-switches-test)
    set_tests_properties(shell-requested-switches PROPERTIES TIMEOUT 60)

    # The brightness model against a fake compositor socket (docs/brightness.md §9).
    find_package(Qt6 6.5 REQUIRED COMPONENTS Network)
    qt_add_executable(vela-brightness-model-test tests/brightness_model.cpp src/brightness.cpp src/brightness.h)
    target_include_directories(vela-brightness-model-test PRIVATE src)
    target_compile_options(vela-brightness-model-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-brightness-model-test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
    add_test(NAME shell-brightness-model COMMAND vela-brightness-model-test)
    set_tests_properties(shell-brightness-model PROPERTIES TIMEOUT 60)

    qt_add_executable(vela-primary-output-test tests/primaryoutput.cpp ${PROJECT_SOURCE_DIR}/common/primaryoutput.h)
    target_compile_options(vela-primary-output-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-primary-output-test PRIVATE Qt6::Gui Qt6::Test)
    add_test(NAME shell-primary-output COMMAND vela-primary-output-test)
    qt_add_executable(vela-appearance-effects-test tests/appearance_effects.cpp)
    target_compile_options(vela-appearance-effects-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-appearance-effects-test PRIVATE vela-controls Qt6::Gui Qt6::Test)
    add_test(NAME shell-appearance-effects COMMAND vela-appearance-effects-test)
endif()

# Italian translation: strings in the code are in English, Italian lives
# in i18n/vela-shell_it.ts and ends up in the executable (:/i18n/vela-shell_it.qm, see
# shell/src/language.h). After changing texts:
#   cmake --build build --target update_translations
find_package(Qt6 6.7 REQUIRED COMPONENTS LinguistTools)
qt_add_translations(TARGETS vela-shell SOURCE_TARGETS vela-shell vela-bluetooth-client vela-bluetooth-ui
    TS_FILES i18n/vela-shell_it.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_TARGET vela-shell_lupdate
    LRELEASE_TARGET vela-shell_lrelease
    LUPDATE_OPTIONS -source-language en -target-language it -no-obsolete -locations none)
