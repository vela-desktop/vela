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
