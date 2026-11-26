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
    src/batteryalerts.cpp
    src/batteryalerts.h
    src/batterynotices.cpp
    src/batterynotices.h
    src/batterysettings.h
    src/desktopexec.h
    src/mimeapps.h
    src/clipboard.cpp
    src/clipboard.h
    src/snip.cpp
    src/snip.h
    src/shortcutrunner.cpp
    src/shortcutrunner.h
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
    src/updatenotices.cpp
    src/updatenotices.h
    src/network.cpp
    src/network.h
    src/notifications.cpp
    src/upowerbatteries.cpp
    src/upowerbatteries.h
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
    src/launcherentry.cpp
    src/launcherentry.h
    src/taskbarmodel.cpp
    src/tray.cpp
    src/taskbarmodel.h
    src/wallpaperprovider.h
    src/wallpapercolor.h
    src/screensharing.cpp
    src/screensharing.h
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
    FILES ${PROJECT_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml
          ${PROJECT_SOURCE_DIR}/protocols/vela-toplevel-identifier-v1.xml)

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
    qml/TaskbarApps.qml
    qml/StartMenu.qml
    qml/GlassEdge.qml
    qml/tiles.js
    qml/AppTile.qml
    qml/Switcher.qml
    qml/Wallpaper.qml
    qml/NotificationPopups.qml
    qml/NotificationToast.qml
    qml/Menus.qml
    qml/menufilter.js
    qml/Styles.qml
    qml/ContextMenu.qml
    qml/MenuPanel.qml
    qml/MenuTextField.qml
    qml/MenuTextBox.qml
    qml/TextMenu.qml
    qml/ConfirmDialog.qml
    qml/BluetoothDialog.qml
    qml/BluetoothDevices.qml
    qml/SourceChooser.qml
    qml/PropertiesDialog.qml
    qml/CheckBox.qml
    qml/QuickSettings.qml
    qml/QsTile.qml
    qml/QuickSettingsTile.qml
    qml/NotificationCenter.qml
    qml/SmallButton.qml
    qml/NotificationList.qml
    qml/TaskView.qml
    qml/CapsLockOsd.qml
    qml/DesktopOsd.qml
    qml/VolumeOsd.qml
    qml/SnapLayouts.qml
    qml/SnapAssist.qml
    qml/TaskbarPreview.qml
    qml/TrayIcon.qml
    qml/TrayOverflow.qml
    qml/TaskbarTooltip.qml
    qml/FileDialogs.qml
    qml/ClipboardPanel.qml
    qml/ClipboardList.qml
    qml/SnipOverlay.qml
    qml/ShadowRoom.qml)

# The files are in qml/ for tidiness, but in the module they must be at the
# root: so they see each other without imports.
foreach(file IN LISTS VELA_QML_FILES)
    get_filename_component(name ${file} NAME)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()
set_source_files_properties(qml/Menus.qml qml/Styles.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(vela-shell
    URI Vela.Shell
    VERSION 1.0
    # Where Vela.Controls and Vela.Bluetooth.Ui are built, for qmllint.
    IMPORT_PATH ${PROJECT_BINARY_DIR}/common ${PROJECT_BINARY_DIR}/common/bluetooth
    QML_FILES ${VELA_QML_FILES})

target_compile_options(vela-shell PRIVATE -Wall -Wextra)
target_link_libraries(vela-shell PRIVATE vela-alloc
    vela-shortcuts # custom shortcuts' file (common/shortcuts/custom.h)
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
    vela-bluetooth-uiplugin
    vela-updates-client) # update notifications (common/updates, docs/updates.md)

install(TARGETS vela-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The tests (ctest): the Exec key of .desktop files, default apps, the
# volume's steps and the animation effects switch Style reads.
if(VELA_BUILD_TOOLS)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test Network)
    qt_add_executable(vela-shell-controller-test tests/shellcontroller_test.cpp
        src/shellcontroller.cpp src/shellcontroller.h)
    target_include_directories(vela-shell-controller-test PRIVATE src)
    target_compile_options(vela-shell-controller-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-shell-controller-test PRIVATE vela-color
        Qt6::Gui Qt6::DBus Qt6::Network Qt6::Test LayerShellQt::Interface)
    add_test(NAME shell-controller COMMAND vela-shell-controller-test)
    set_tests_properties(shell-controller PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
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

    if(DBUS_RUN_SESSION_EXECUTABLE)
        qt_add_executable(vela-launcherentry-test tests/launcherentry_test.cpp src/launcherentry.cpp src/launcherentry.h)
        target_include_directories(vela-launcherentry-test PRIVATE src)
        target_compile_options(vela-launcherentry-test PRIVATE -Wall -Wextra)
        target_link_libraries(vela-launcherentry-test PRIVATE Qt6::DBus Qt6::Test)
        add_test(NAME shell-launcherentry COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:vela-launcherentry-test>)

        # The notification area: the shell's watcher and fake apps' icons.
        qt_add_executable(vela-tray-test tests/tray_test.cpp src/tray.cpp src/tray.h
            ${PROJECT_SOURCE_DIR}/common/tests/sniproperties.h)
        target_include_directories(vela-tray-test PRIVATE src ${PROJECT_SOURCE_DIR}/common/tests)
        target_compile_options(vela-tray-test PRIVATE -Wall -Wextra)
        target_link_libraries(vela-tray-test PRIVATE Qt6::DBus Qt6::Quick Qt6::Test)
        add_test(NAME shell-tray COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:vela-tray-test>)
        set_tests_properties(shell-tray PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()

    # Battery notifications (docs/battery-notifications.md §9): the policy
    # and UPower's properties read into it, with no D-Bus...
    qt_add_executable(vela-battery-alerts-test tests/batteryalerts.cpp
        src/batteryalerts.cpp src/batteryalerts.h src/upowerbatteries.cpp src/upowerbatteries.h)
    target_include_directories(vela-battery-alerts-test PRIVATE src)
    target_compile_options(vela-battery-alerts-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-battery-alerts-test PRIVATE Qt6::DBus Qt6::Test)
    add_test(NAME shell-battery-alerts COMMAND vela-battery-alerts-test)
    # ...and all of it against a fake UPower on a private bus, down to the
    # notifications shown.
    qt_add_executable(vela-battery-notices-test tests/batterynotices.cpp
        src/batteryalerts.cpp src/batteryalerts.h src/batterynotices.cpp src/batterynotices.h
        src/upowerbatteries.cpp src/upowerbatteries.h src/notifications.cpp src/notifications.h)
    target_include_directories(vela-battery-notices-test PRIVATE src)
    target_compile_options(vela-battery-notices-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-battery-notices-test PRIVATE Qt6::DBus Qt6::Gui Qt6::Quick Qt6::Test)
    add_test(NAME shell-battery-notices COMMAND vela-battery-notices-test)
    set_tests_properties(shell-battery-notices PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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

    qt_add_executable(vela-servicemenus-test tests/servicemenus.cpp src/servicemenus.cpp src/servicemenus.h)
    target_include_directories(vela-servicemenus-test PRIVATE src)
    target_compile_options(vela-servicemenus-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-servicemenus-test PRIVATE vela-process Qt6::Core Qt6::Test)
    add_test(NAME shell-service-menus COMMAND vela-servicemenus-test)
    set_tests_properties(shell-service-menus PROPERTIES TIMEOUT 15)

    qt_add_executable(vela-mimeapps-test tests/mimeapps.cpp src/mimeapps.h)
    target_include_directories(vela-mimeapps-test PRIVATE src)
    target_compile_options(vela-mimeapps-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-mimeapps-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME shell-mimeapps COMMAND vela-mimeapps-test)

    qt_add_executable(vela-themefile-test tests/themefile.cpp src/themefile.h)
    target_include_directories(vela-themefile-test PRIVATE src)
    target_compile_definitions(vela-themefile-test PRIVATE
        THEMES_DIR="${PROJECT_SOURCE_DIR}/themes"
        THEME_QML="${PROJECT_SOURCE_DIR}/common/controls/Theme.qml"
        STYLES_QML="${PROJECT_SOURCE_DIR}/shell/qml/Styles.qml"
        THEMEFILE_H="${CMAKE_CURRENT_SOURCE_DIR}/src/themefile.h")
    target_compile_options(vela-themefile-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-themefile-test PRIVATE vela-theme-tokens Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME shell-themefile COMMAND vela-themefile-test)

    # The shell's own pieces (tests/tst_*.qml) under Qt Quick Test, with fakes
    # of what its windows find in their context (Vela.Bluetooth, Shell,
    # BluetoothPrompts), with animation effects on and off.
    find_package(Qt6 6.5 REQUIRED COMPONENTS QuickTest)
    qt_add_executable(vela-shell-test tests/shell_test.cpp ${PROJECT_SOURCE_DIR}/common/tests/accessibleprobe.h)
    target_compile_definitions(vela-shell-test PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
        FAKE_BLUETOOTH="${PROJECT_SOURCE_DIR}/common/bluetooth/tests/FakeBluetooth.qml")
    target_compile_options(vela-shell-test PRIVATE -Wall -Wextra)
    target_include_directories(vela-shell-test PRIVATE ${PROJECT_SOURCE_DIR}/common/tests)
    target_link_libraries(vela-shell-test PRIVATE vela-controls vela-controlsplugin vela-bluetooth-uiplugin Qt6::QuickTest)
    add_test(NAME shell-controls COMMAND vela-shell-test)
    add_test(NAME shell-controls-no-effects COMMAND vela-shell-test)
    set_tests_properties(shell-controls-no-effects PROPERTIES ENVIRONMENT "VELA_TEST_ANIMATION_EFFECTS=no")
    set_tests_properties(shell-controls shell-controls-no-effects PROPERTIES TIMEOUT 60)
    # Under a theme of expressive controls and motion (common/tests/quicktest.h,
    # VELA_TEST_THEME), on tests/expressive/ only.
    add_test(NAME shell-controls-expressive COMMAND vela-shell-test -input ${CMAKE_CURRENT_SOURCE_DIR}/tests/expressive)
    add_test(NAME shell-controls-expressive-no-effects COMMAND vela-shell-test -input ${CMAKE_CURRENT_SOURCE_DIR}/tests/expressive)
    set_tests_properties(shell-controls-expressive PROPERTIES ENVIRONMENT "VELA_TEST_THEME=expressive")
    set_tests_properties(shell-controls-expressive-no-effects PROPERTIES
        ENVIRONMENT "VELA_TEST_THEME=expressive;VELA_TEST_ANIMATION_EFFECTS=no")
    set_tests_properties(shell-controls-expressive shell-controls-expressive-no-effects PROPERTIES TIMEOUT 60)
    # Each expressive piece alone inside the default theme (VELA_TEST_THEME=
    # shell, controls): the mixed combinations; motion and color alone are
    # the controls suite's (common/CMakeLists.txt).
    foreach(piece shell controls)
        add_test(NAME shell-controls-expressive-${piece} COMMAND vela-shell-test -input ${CMAKE_CURRENT_SOURCE_DIR}/tests/expressive)
        set_tests_properties(shell-controls-expressive-${piece} PROPERTIES TIMEOUT 60 ENVIRONMENT "VELA_TEST_THEME=${piece}")
    endforeach()

    # Exercise the real taskbar layout and pointer targets with private services.
    qt_add_executable(vela-taskbar-test tests/taskbar_test.cpp src/config.cpp src/config.h
        ${PROJECT_SOURCE_DIR}/common/tests/accessibleprobe.h)
    target_compile_definitions(vela-taskbar-test PRIVATE QUICK_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}/tests/taskbar"
        SHELL_STYLE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/qml/Styles.qml")
    target_compile_options(vela-taskbar-test PRIVATE -Wall -Wextra)
    target_include_directories(vela-taskbar-test PRIVATE ${PROJECT_SOURCE_DIR}/common/tests src)
    target_link_libraries(vela-taskbar-test PRIVATE vela-controls vela-controlsplugin Qt6::QuickTest)
    # Compile the production pieces unchanged, with private Styles and Menus
    # singletons in their module (directory imports cannot resolve singletons).
    set(taskbar_fakes ${PROJECT_SOURCE_DIR}/tests/taskbar/Styles.qml ${PROJECT_SOURCE_DIR}/tests/taskbar/Menus.qml)
    foreach(file IN LISTS taskbar_fakes)
        get_filename_component(name ${file} NAME)
        set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name} QT_QML_SINGLETON_TYPE TRUE)
    endforeach()
    qt_add_qml_module(vela-taskbar-test URI Vela.Taskbar.Test VERSION 1.0
        IMPORT_PATH ${PROJECT_BINARY_DIR}/common
        QML_FILES qml/Taskbar.qml qml/TaskbarApps.qml qml/TaskbarButton.qml qml/TrayIcon.qml qml/TrayOverflow.qml
        qml/NotificationCenter.qml qml/NotificationList.qml qml/SmallButton.qml qml/ShadowRoom.qml ${taskbar_fakes})
    add_test(NAME shell-taskbar COMMAND vela-taskbar-test)
    add_test(NAME shell-taskbar-no-effects COMMAND vela-taskbar-test)
    set_tests_properties(shell-taskbar-no-effects PROPERTIES ENVIRONMENT "VELA_TEST_ANIMATION_EFFECTS=no")
    set_tests_properties(shell-taskbar shell-taskbar-no-effects PROPERTIES TIMEOUT 60)

    # The production taskbar and notification timestamps after real atomic
    # settings writes, independent of acrylic composition and the GPU driver.
    add_test(NAME shell-clock-formats COMMAND vela-taskbar-test -input ${PROJECT_SOURCE_DIR}/tests/clock)
    set_tests_properties(shell-clock-formats PROPERTIES TIMEOUT 60 ENVIRONMENT "VELA_TEST_SHELL_CONFIG=real")

    # The taskbar's buttons from windows no compositor backs (ForeignToplevel
    # with no handle), apps in a private home and badges on a private bus.
    if(DBUS_RUN_SESSION_EXECUTABLE)
        qt_add_executable(vela-taskbarmodel-test tests/taskbarmodel_test.cpp
            src/taskbarmodel.cpp src/taskbarmodel.h src/foreigntoplevels.cpp src/foreigntoplevels.h
            src/appmodel.cpp src/appmodel.h src/desktopexec.h src/mimeapps.h
            src/config.cpp src/config.h src/batterysettings.h src/clockformat.h src/starticon.h src/defaults.h
            src/launcherentry.cpp src/launcherentry.h src/workspaces.cpp src/workspaces.h
            ${PROJECT_SOURCE_DIR}/common/tests/privatehome.h)
        # The protocol code generated for vela-shell, in this same directory:
        # after it, or Make compiles the test before the headers exist.
        add_dependencies(vela-taskbarmodel-test vela-shell)
        foreach(protocol wlr-foreign-toplevel-management-unstable-v1 vela-toplevel-identifier-v1)
            target_sources(vela-taskbarmodel-test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/qwayland-${protocol}.cpp
                ${CMAKE_CURRENT_BINARY_DIR}/wayland-${protocol}-protocol.c)
        endforeach()
        target_include_directories(vela-taskbarmodel-test PRIVATE ${PROJECT_SOURCE_DIR}/common/tests src
            ${CMAKE_CURRENT_BINARY_DIR})
        target_compile_options(vela-taskbarmodel-test PRIVATE -Wall -Wextra)
        target_link_libraries(vela-taskbarmodel-test PRIVATE vela-process Qt6::DBus Qt6::Gui Qt6::GuiPrivate Qt6::Network
            Qt6::WaylandClient Qt6::Test)
        add_test(NAME shell-taskbarmodel COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:vela-taskbarmodel-test>)
        set_tests_properties(shell-taskbarmodel PROPERTIES TIMEOUT 60)
    endif()

    # Config on a private vela-shell.conf, rewritten as Settings does.
    qt_add_executable(vela-config-test tests/config_test.cpp src/config.cpp src/config.h
        src/batterysettings.h src/clockformat.h src/starticon.h src/defaults.h)
    target_include_directories(vela-config-test PRIVATE ${PROJECT_SOURCE_DIR}/common/tests src)
    target_compile_options(vela-config-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-config-test PRIVATE Qt6::Gui Qt6::Test)
    add_test(NAME shell-config COMMAND vela-config-test)
    set_tests_properties(shell-config PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Clipboard history in a private home, offscreen: no compositor, so the
    # list, its images and the pinned files alone.
    qt_add_executable(vela-clipboard-test tests/clipboard_test.cpp src/clipboard.cpp src/clipboard.h
        ${PROJECT_SOURCE_DIR}/common/tests/privatehome.h)
    # The ext-data-control code generated for vela-shell: after it, as above.
    add_dependencies(vela-clipboard-test vela-shell)
    target_sources(vela-clipboard-test PRIVATE ${SHELL_GENERATED_DIR}/ext-data-control-v1-protocol.c)
    target_include_directories(vela-clipboard-test PRIVATE ${PROJECT_SOURCE_DIR}/common/tests src
        ${SHELL_GENERATED_DIR})
    target_compile_options(vela-clipboard-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-clipboard-test PRIVATE Qt6::DBus Qt6::Gui Qt6::Quick Qt6::Test
        PkgConfig::SHELL_WAYLAND_CLIENT)
    add_test(NAME shell-clipboard COMMAND vela-clipboard-test)
    set_tests_properties(shell-clipboard PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(vela-start-icon-test tests/starticon.cpp src/starticon.h)
    target_include_directories(vela-start-icon-test PRIVATE src)
    target_compile_options(vela-start-icon-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-start-icon-test PRIVATE Qt6::Gui Qt6::Test)
    add_test(NAME shell-start-icon COMMAND vela-start-icon-test)
    set_tests_properties(shell-start-icon PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(vela-icon-provider-test tests/iconprovider.cpp src/iconprovider.h)
    target_include_directories(vela-icon-provider-test PRIVATE src)
    target_compile_options(vela-icon-provider-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-icon-provider-test PRIVATE Qt6::Quick Qt6::DBus Qt6::Test)
    add_test(NAME shell-icon-provider COMMAND vela-icon-provider-test)
    set_tests_properties(shell-icon-provider PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(vela-tool-path-test tests/velatool.cpp src/velatool.h)
    target_include_directories(vela-tool-path-test PRIVATE src)
    target_compile_options(vela-tool-path-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-tool-path-test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME shell-tool-path COMMAND vela-tool-path-test)

    qt_add_executable(vela-appmodel-watch-test
        tests/appmodel_watch.cpp src/appmodel.cpp src/appmodel.h src/desktopexec.h src/mimeapps.h)
    target_include_directories(vela-appmodel-watch-test PRIVATE src)
    target_compile_options(vela-appmodel-watch-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-appmodel-watch-test PRIVATE vela-process Qt6::Core Qt6::Test)
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
    target_link_libraries(vela-service-models-test PRIVATE vela-process Qt6::Core Qt6::DBus Qt6::Test)
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
    # Do not disturb by itself while the screen is shared (docs/screen-sharing.md §7).
    qt_add_executable(vela-notification-silence-test tests/notification_silence.cpp src/notifications.cpp
        src/notifications.h)
    target_include_directories(vela-notification-silence-test PRIVATE ${PROJECT_SOURCE_DIR}/common/tests src)
    target_compile_options(vela-notification-silence-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-notification-silence-test PRIVATE Qt6::DBus Qt6::Gui Qt6::Quick Qt6::Test)
    add_test(NAME shell-notification-silence COMMAND vela-notification-silence-test)
    set_tests_properties(shell-notification-silence PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # What a client on the bus can make the shell show and keep.
    qt_add_executable(vela-notification-limits-test tests/notification_limits.cpp src/notifications.cpp
        src/notifications.h)
    target_include_directories(vela-notification-limits-test PRIVATE ${PROJECT_SOURCE_DIR}/common/tests src)
    target_compile_options(vela-notification-limits-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-notification-limits-test PRIVATE Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test)
    add_test(NAME shell-notification-limits COMMAND vela-notification-limits-test)
    set_tests_properties(shell-notification-limits PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    # "image-data" from the bus: refused when it doesn't hold its own rows.
    qt_add_executable(vela-notification-image-test tests/notification_image.cpp src/notifications.cpp
        src/notifications.h)
    target_include_directories(vela-notification-image-test PRIVATE src)
    target_compile_options(vela-notification-image-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-notification-image-test PRIVATE Qt6::DBus Qt6::Gui Qt6::Quick Qt6::Test)
    if(DBUS_RUN_SESSION_EXECUTABLE)
        add_test(NAME shell-notification-image COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:vela-notification-image-test>)
    else()
        add_test(NAME shell-notification-image COMMAND vela-notification-image-test)
    endif()
    set_tests_properties(shell-notification-image PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Who shares the screen, named, against a fake compositor (docs/screen-sharing.md §5).
    qt_add_executable(vela-screen-sharing-test tests/screensharing.cpp src/screensharing.cpp src/screensharing.h)
    target_include_directories(vela-screen-sharing-test PRIVATE src)
    target_compile_options(vela-screen-sharing-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-screen-sharing-test PRIVATE Qt6::Gui Qt6::Network Qt6::Test)
    add_test(NAME shell-screen-sharing COMMAND vela-screen-sharing-test)
    set_tests_properties(shell-screen-sharing PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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
    TS_FILES i18n/vela-shell_it.ts i18n/vela-shell_pt.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_TARGET vela-shell_lupdate
    LRELEASE_TARGET vela-shell_lrelease
    LUPDATE_OPTIONS -source-language en -target-language it -no-obsolete -locations none)
