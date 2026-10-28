# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-bluetooth against a fake BlueZ on private buses (docs/bluetooth.md
# §11). The fake is Qt: it's test code. The executable's path is the UI
# the test build of the daemon trusts (bluetooth/CMakeLists.txt).
find_package(Qt6 6.5 REQUIRED COMPONENTS DBus Test)
qt_add_executable(vela-bluetooth-daemon-test daemon.cpp fakebluez.h)
set_target_properties(vela-bluetooth-daemon-test PROPERTIES AUTOMOC ON)
target_compile_options(vela-bluetooth-daemon-test PRIVATE -Wall -Wextra)
target_compile_definitions(vela-bluetooth-daemon-test PRIVATE
    VELA_BLUETOOTH_DAEMON="$<TARGET_FILE:vela-bluetooth-testing>")
target_link_libraries(vela-bluetooth-daemon-test PRIVATE Qt6::DBus Qt6::Test)
add_dependencies(vela-bluetooth-daemon-test vela-bluetooth-testing)
add_test(NAME bluetooth-daemon COMMAND vela-bluetooth-daemon-test)
set_tests_properties(bluetooth-daemon PROPERTIES TIMEOUT 180)

# Vela.Bluetooth's model (common/bluetooth) against the same daemon and fake;
# also a UI the test build trusts.
qt_add_executable(vela-bluetooth-client-test client.cpp fakebluez.h)
set_target_properties(vela-bluetooth-client-test PROPERTIES AUTOMOC ON)
target_compile_options(vela-bluetooth-client-test PRIVATE -Wall -Wextra)
target_compile_definitions(vela-bluetooth-client-test PRIVATE
    VELA_BLUETOOTH_DAEMON="$<TARGET_FILE:vela-bluetooth-testing>")
target_link_libraries(vela-bluetooth-client-test PRIVATE vela-bluetooth-client Qt6::DBus Qt6::Test)
add_dependencies(vela-bluetooth-client-test vela-bluetooth-testing)
add_test(NAME bluetooth-client COMMAND vela-bluetooth-client-test)
set_tests_properties(bluetooth-client PROPERTIES TIMEOUT 120)

# The shell's side (§9.2, §9.3): the questions it draws as the dialog or a
# notification, and airplane mode; also a UI the test build trusts.
find_package(Qt6 6.5 REQUIRED COMPONENTS Quick)
qt_add_executable(vela-bluetooth-shell-test shell.cpp fakebluez.h
    ${PROJECT_SOURCE_DIR}/shell/src/bluetoothprompts.cpp
    ${PROJECT_SOURCE_DIR}/shell/src/bluetoothprompts.h
    ${PROJECT_SOURCE_DIR}/shell/src/notifications.cpp
    ${PROJECT_SOURCE_DIR}/shell/src/notifications.h
    ${PROJECT_SOURCE_DIR}/shell/src/systemstatus.cpp
    ${PROJECT_SOURCE_DIR}/shell/src/systemstatus.h)
set_target_properties(vela-bluetooth-shell-test PROPERTIES AUTOMOC ON)
target_include_directories(vela-bluetooth-shell-test PRIVATE ${PROJECT_SOURCE_DIR}/shell/src)
target_compile_options(vela-bluetooth-shell-test PRIVATE -Wall -Wextra)
target_compile_definitions(vela-bluetooth-shell-test PRIVATE
    VELA_BLUETOOTH_DAEMON="$<TARGET_FILE:vela-bluetooth-testing>")
target_link_libraries(vela-bluetooth-shell-test PRIVATE vela-bluetooth-client Qt6::DBus Qt6::Quick Qt6::Test)
add_dependencies(vela-bluetooth-shell-test vela-bluetooth-testing)
add_test(NAME bluetooth-shell COMMAND vela-bluetooth-shell-test)
set_tests_properties(bluetooth-shell PROPERTIES TIMEOUT 120)
