# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-update-daemon on private buses, with a fake vela-update, terminal
# and NetworkManager (docs/updates.md §10).
find_package(Qt6 6.5 REQUIRED COMPONENTS DBus Test)
qt_add_executable(vela-update-daemon-test daemon.cpp harness.h)
set_target_properties(vela-update-daemon-test PROPERTIES AUTOMOC ON)
target_compile_options(vela-update-daemon-test PRIVATE -Wall -Wextra)
target_compile_definitions(vela-update-daemon-test PRIVATE
    VELA_UPDATE_DAEMON="$<TARGET_FILE:vela-update-daemon-testing>"
    VELA_UPDATE_STATE="${PROJECT_SOURCE_DIR}/packaging/vela-update-state")
target_link_libraries(vela-update-daemon-test PRIVATE Qt6::DBus Qt6::Test)
add_dependencies(vela-update-daemon-test vela-update-daemon-testing)
add_test(NAME updates-daemon COMMAND vela-update-daemon-test)
set_tests_properties(updates-daemon PROPERTIES TIMEOUT 180)

# vela-updates-client (common/updates) against the same daemon and fakes.
qt_add_executable(vela-updates-client-test client.cpp harness.h)
set_target_properties(vela-updates-client-test PROPERTIES AUTOMOC ON)
target_compile_options(vela-updates-client-test PRIVATE -Wall -Wextra)
target_compile_definitions(vela-updates-client-test PRIVATE
    VELA_UPDATE_DAEMON="$<TARGET_FILE:vela-update-daemon-testing>"
    VELA_UPDATE_STATE="${PROJECT_SOURCE_DIR}/packaging/vela-update-state")
target_link_libraries(vela-updates-client-test PRIVATE vela-updates-client Qt6::DBus Qt6::Test)
add_dependencies(vela-updates-client-test vela-update-daemon-testing)
add_test(NAME updates-client COMMAND vela-updates-client-test)
set_tests_properties(updates-client PROPERTIES TIMEOUT 120)

# The shell's notifications (shell/src/updatenotices.cpp) on the same
# daemon and fakes.
find_package(Qt6 6.5 REQUIRED COMPONENTS Quick)
qt_add_executable(vela-updates-shell-test shell.cpp harness.h
    ${PROJECT_SOURCE_DIR}/shell/src/notifications.cpp
    ${PROJECT_SOURCE_DIR}/shell/src/notifications.h
    ${PROJECT_SOURCE_DIR}/shell/src/updatenotices.cpp
    ${PROJECT_SOURCE_DIR}/shell/src/updatenotices.h)
set_target_properties(vela-updates-shell-test PROPERTIES AUTOMOC ON)
target_include_directories(vela-updates-shell-test PRIVATE ${PROJECT_SOURCE_DIR}/shell/src)
target_compile_options(vela-updates-shell-test PRIVATE -Wall -Wextra)
target_compile_definitions(vela-updates-shell-test PRIVATE
    VELA_UPDATE_DAEMON="$<TARGET_FILE:vela-update-daemon-testing>"
    VELA_UPDATE_STATE="${PROJECT_SOURCE_DIR}/packaging/vela-update-state")
target_link_libraries(vela-updates-shell-test PRIVATE vela-updates-client Qt6::DBus Qt6::Quick Qt6::Test)
add_dependencies(vela-updates-shell-test vela-update-daemon-testing)
add_test(NAME updates-shell COMMAND vela-updates-shell-test)
set_tests_properties(updates-shell PROPERTIES TIMEOUT 120)
