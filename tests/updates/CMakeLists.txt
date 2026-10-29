# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-update-daemon on private buses, with a fake vela-update, terminal
# and NetworkManager (docs/updates.md §10).
find_package(Qt6 6.5 REQUIRED COMPONENTS DBus Test)
qt_add_executable(vela-update-daemon-test daemon.cpp)
set_target_properties(vela-update-daemon-test PROPERTIES AUTOMOC ON)
target_compile_options(vela-update-daemon-test PRIVATE -Wall -Wextra)
target_compile_definitions(vela-update-daemon-test PRIVATE
    VELA_UPDATE_DAEMON="$<TARGET_FILE:vela-update-daemon-testing>"
    VELA_UPDATE_STATE="${PROJECT_SOURCE_DIR}/packaging/vela-update-state")
target_link_libraries(vela-update-daemon-test PRIVATE Qt6::DBus Qt6::Test)
add_dependencies(vela-update-daemon-test vela-update-daemon-testing)
add_test(NAME updates-daemon COMMAND vela-update-daemon-test)
set_tests_properties(updates-daemon PROPERTIES TIMEOUT 180)
