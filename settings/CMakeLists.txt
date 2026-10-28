# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# Vela's Settings app: Qt Quick, like the shell, whose pieces it partly reuses
# (installed apps, system programs, system status).
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS DBus Gui GuiPrivate Network Qml Quick QuickControls2 Svg)

qt_standard_project_setup(REQUIRES 6.5)

# Output configuration is native and built even when test tools are disabled.
find_package(PkgConfig REQUIRED)
pkg_check_modules(DISPLAYS_WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
find_program(DISPLAYS_WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
set(DISPLAYS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${DISPLAYS_GENERATED_DIR})
set(DISPLAYS_PROTOCOL_SOURCES)
foreach(name wlr-output-management-unstable-v1 vela-output-preview-v1)
    set(xml ${PROJECT_SOURCE_DIR}/protocols/${name}.xml)
    set(header ${DISPLAYS_GENERATED_DIR}/${name}-client-protocol.h)
    set(code ${DISPLAYS_GENERATED_DIR}/${name}-protocol.c)
    add_custom_command(OUTPUT ${header} ${code}
        COMMAND ${DISPLAYS_WAYLAND_SCANNER} client-header ${xml} ${header}
        COMMAND ${DISPLAYS_WAYLAND_SCANNER} private-code ${xml} ${code}
        DEPENDS ${xml})
    list(APPEND DISPLAYS_PROTOCOL_SOURCES ${header} ${code})
endforeach()
add_library(vela-displays STATIC src/displays.cpp src/displays.h ${DISPLAYS_PROTOCOL_SOURCES})
target_include_directories(vela-displays PUBLIC src PRIVATE ${DISPLAYS_GENERATED_DIR})
target_link_libraries(vela-displays PUBLIC Qt6::Gui PRIVATE PkgConfig::DISPLAYS_WAYLAND)
target_compile_options(vela-displays PRIVATE -Wall -Wextra)

set(SHELL_SRC ${PROJECT_SOURCE_DIR}/shell/src)

qt_add_executable(vela-settings
    src/main.cpp
    src/about.cpp
    src/about.h
    src/datetime.cpp
    src/datetime.h
    src/defaultapps.cpp
    src/defaultapps.h
    src/keyboardlayouts.cpp
    src/keyboardlayouts.h
    src/preferences.cpp
    src/preferences.h
    ${PROJECT_SOURCE_DIR}/compositor/src/color.c # night light's temperature, read as the compositor does
    ${PROJECT_SOURCE_DIR}/compositor/src/color.h
    ${PROJECT_SOURCE_DIR}/compositor/src/legacy_names.c
    ${PROJECT_SOURCE_DIR}/compositor/src/legacy_names.h
    ${SHELL_SRC}/appmodel.cpp
    ${SHELL_SRC}/appmodel.h
    ${SHELL_SRC}/audioapps.h
    ${SHELL_SRC}/iconprovider.h
    ${SHELL_SRC}/mica.h
    ${SHELL_SRC}/network.cpp
    ${SHELL_SRC}/network.h
    ${SHELL_SRC}/systemactions.cpp
    ${SHELL_SRC}/systemactions.h
    ${SHELL_SRC}/systemstatus.cpp
    ${SHELL_SRC}/systemstatus.h
    ${SHELL_SRC}/brightness.cpp
    ${SHELL_SRC}/brightness.h)
target_include_directories(vela-settings PRIVATE ${SHELL_SRC} ${PROJECT_SOURCE_DIR}/compositor/src)

set(SETTINGS_QML_FILES
    qml/Main.qml
    qml/Card.qml
    qml/BrightnessCard.qml
    qml/CardGroup.qml
    qml/LinkCard.qml
    qml/Choice.qml
    qml/RadioButton.qml
    qml/LevelBar.qml
    qml/MoreMenu.qml
    qml/BluetoothDevice.qml
    qml/AddDeviceDialog.qml
    qml/StillVisibleCard.qml
    qml/PageHeader.qml
    qml/Page.qml
    qml/pages/HomePage.qml
    qml/pages/SystemPage.qml
    qml/pages/DisplayPage.qml
    qml/pages/SoundPage.qml
    qml/pages/SoundDevicePage.qml
    qml/pages/VolumeMixerPage.qml
    qml/pages/NotificationsPage.qml
    qml/pages/PowerPage.qml
    qml/pages/AboutPage.qml
    qml/pages/BluetoothPage.qml
    qml/pages/NetworkPage.qml
    qml/pages/PersonalizationPage.qml
    qml/pages/BackgroundPage.qml
    qml/pages/ColorsPage.qml
    qml/pages/TaskbarPage.qml
    qml/pages/AppsPage.qml
    qml/pages/InstalledAppsPage.qml
    qml/pages/DefaultAppsPage.qml
    qml/pages/DefaultAppPage.qml
    qml/pages/TimeLanguagePage.qml
    qml/pages/DateTimePage.qml
    qml/pages/KeyboardPage.qml
    qml/pages/NightLightPage.qml
    qml/pages/AccessibilityPage.qml
    qml/pages/MousePage.qml
    qml/pages/TouchpadPage.qml
    qml/pages/ClipboardPage.qml)

foreach(file IN LISTS SETTINGS_QML_FILES)
    get_filename_component(name ${file} NAME)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()

qt_add_qml_module(vela-settings
    URI Vela.Settings
    VERSION 1.0
    QML_FILES ${SETTINGS_QML_FILES})

target_compile_options(vela-settings PRIVATE -Wall -Wextra)
target_compile_definitions(vela-settings PRIVATE VELA_VERSION="${PROJECT_VERSION}")

# The shell's default wallpaper, to show it and compute its Mica color.
qt_add_resources(vela-settings "images"
    PREFIX "/vela"
    BASE ${PROJECT_SOURCE_DIR}
    FILES ${PROJECT_SOURCE_DIR}/images/vela_splash_169.svg)
target_link_libraries(vela-settings PRIVATE
    vela-displays
    Qt6::DBus
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
    vela-controls # Theme, Button, TextBox, Style (common/)
    vela-controlsplugin
    vela-app # the mouse's Back and Forward buttons (common/)
    vela-audio # the sound (common/audio, docs/audio.md)
    vela-bluetooth-client # Bluetooth (common/bluetooth, docs/bluetooth.md)
    vela-bluetooth-uiplugin)

install(TARGETS vela-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES vela-settings.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# Italian translation: strings in the code are in English, Italian lives
# in i18n/vela-settings_it.ts and ends up in the executable (:/i18n/vela-settings_it.qm, see
# shell/src/language.h). After changing texts:
#   cmake --build build --target update_translations
find_package(Qt6 6.7 REQUIRED COMPONENTS LinguistTools)
qt_add_translations(TARGETS vela-settings SOURCE_TARGETS vela-settings vela-displays vela-controls vela-bluetooth-client vela-bluetooth-ui
    TS_FILES i18n/vela-settings_it.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_TARGET vela-settings_lupdate
    LRELEASE_TARGET vela-settings_lrelease
    LUPDATE_OPTIONS -source-language en -target-language it -no-obsolete -locations none)

# Settings' own pieces (tests/tst_*.qml) under Qt Quick Test, with a fake
# Vela.Bluetooth, with animation effects on and off.
if(VELA_BUILD_TOOLS)
    find_package(Qt6 6.5 REQUIRED COMPONENTS QuickTest)
    qt_add_executable(vela-settings-test tests/settings_test.cpp)
    target_compile_definitions(vela-settings-test PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
        FAKE_BLUETOOTH="${PROJECT_SOURCE_DIR}/common/bluetooth/tests/FakeBluetooth.qml")
    target_compile_options(vela-settings-test PRIVATE -Wall -Wextra)
    target_include_directories(vela-settings-test PRIVATE ${PROJECT_SOURCE_DIR}/common/tests)
    target_link_libraries(vela-settings-test PRIVATE vela-controls vela-controlsplugin vela-bluetooth-uiplugin Qt6::QuickTest)
    add_test(NAME settings-controls COMMAND vela-settings-test)
    add_test(NAME settings-controls-no-effects COMMAND vela-settings-test)
    set_tests_properties(settings-controls-no-effects PROPERTIES ENVIRONMENT "VELA_TEST_ANIMATION_EFFECTS=no")
    set_tests_properties(settings-controls settings-controls-no-effects PROPERTIES TIMEOUT 60)
endif()

if(VELA_BUILD_TOOLS AND VELA_BUILD_COMPOSITOR)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    qt_add_executable(vela-displays-test tests/displays_test.cpp)
    target_include_directories(vela-displays-test PRIVATE ${DISPLAYS_GENERATED_DIR})
    target_link_libraries(vela-displays-test PRIVATE vela-displays Qt6::Test PkgConfig::DISPLAYS_WAYLAND)
    target_compile_options(vela-displays-test PRIVATE -Wall -Wextra)
    target_link_options(vela-displays-test PRIVATE
        -Wl,--wrap=wl_display_flush -Wl,--wrap=wl_display_dispatch -Wl,--wrap=wl_display_disconnect)
    add_dependencies(vela-displays-test vela-display-test-server)
    add_test(NAME settings-displays COMMAND vela-displays-test $<TARGET_FILE:vela-display-test-server>)
    set_tests_properties(settings-displays PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()
