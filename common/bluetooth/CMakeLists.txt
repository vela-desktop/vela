# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-bluetooth-client (docs/bluetooth.md §8): the shell's and Settings'
# side of vela-bluetooth, over D-Bus. Without the daemon it is just empty.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus)

add_library(vela-bluetooth-client STATIC
    bluetooth.cpp
    bluetooth.h)
set_target_properties(vela-bluetooth-client PROPERTIES AUTOMOC ON)
# requested.h: the switch shown at once (shell/src).
# Included as "bluetooth/bluetooth.h": the compositor has a bluetooth.h too.
target_include_directories(vela-bluetooth-client PUBLIC ${PROJECT_SOURCE_DIR}/common ${PROJECT_SOURCE_DIR}/shell/src)
target_compile_options(vela-bluetooth-client PRIVATE -Wall -Wextra)
target_link_libraries(vela-bluetooth-client PUBLIC Qt6::Core Qt6::DBus)

# Vela.Bluetooth.Ui: what Settings and the shell draw alike, a device's
# state in words (DeviceState) and the agent's questions
# (BluetoothQuestion, §9.3).
find_package(Qt6 6.5 REQUIRED COMPONENTS Qml Quick)
set_source_files_properties(qml/DeviceState.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(vela-bluetooth-ui
    URI Vela.Bluetooth.Ui
    VERSION 1.0
    STATIC
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Vela/Bluetooth/Ui
    QML_FILES
        qml/BluetoothQuestion.qml
        qml/DeviceState.qml
    DEPENDENCIES Vela.Controls)
target_link_libraries(vela-bluetooth-ui PUBLIC Qt6::Qml Qt6::Quick vela-controls)

# The tests are in tests/bluetooth, next to the fake BlueZ, and in Settings'
# QML tests.
