# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-bluetooth, the session daemon that talks to BlueZ
# (docs/bluetooth.md §6). C17 with sd-bus and sd-event from libsystemd: no
# Qt, no GLib.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(BLUETOOTH_SYSTEMD IMPORTED_TARGET libsystemd)
endif()
if(NOT BLUETOOTH_SYSTEMD_FOUND)
    message(STATUS "libsystemd not found: no vela-bluetooth")
    return()
endif()

add_library(vela-bluetooth-core STATIC
    src/agent.c
    src/bluez.c
    src/describe.c
    src/model.c
    src/operations.c
    src/outcome.c
    src/power.c
    src/question.c
    src/rfkill.c
    src/search.c
    src/service.c
    src/session.c)
target_include_directories(vela-bluetooth-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(vela-bluetooth-core PUBLIC _GNU_SOURCE)
target_compile_options(vela-bluetooth-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(vela-bluetooth-core PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
# vela.conf (the adapter the user chose) is read and written as the
# compositor does. Who may call (§7.3): common/caller.
target_link_libraries(vela-bluetooth-core PUBLIC vela-pure vela-caller PkgConfig::BLUETOOTH_SYSTEMD)

# Vela's UI (§7.3): where vela-shell and vela-settings are installed, and
# where they are built, trusted only by a daemon run from its build
# directory.
add_executable(vela-bluetooth src/main.c)
target_compile_options(vela-bluetooth PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(vela-bluetooth PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(vela-bluetooth PRIVATE vela-bluetooth-core)
target_compile_definitions(vela-bluetooth PRIVATE
    VELA_BT_SHELL="${CMAKE_INSTALL_FULL_BINDIR}/vela-shell"
    VELA_BT_SETTINGS="${CMAKE_INSTALL_FULL_BINDIR}/vela-settings"
    VELA_BT_BUILD_SELF="${CMAKE_CURRENT_BINARY_DIR}/vela-bluetooth"
    VELA_BT_BUILD_SHELL="${CMAKE_BINARY_DIR}/shell/vela-shell"
    VELA_BT_BUILD_SETTINGS="${CMAKE_BINARY_DIR}/settings/vela-settings")
install(TARGETS vela-bluetooth RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

if(VELA_BUILD_TOOLS)
    # The daemon the tests run (§11): it trusts the tests' UI clients (the
    # daemon's tests, the client library's and the shell's) and takes a fake /dev/rfkill
    # and /sys. Never installed.
    add_executable(vela-bluetooth-testing src/main.c)
    target_compile_options(vela-bluetooth-testing PRIVATE -Wall -Wextra -Wno-unused-parameter)
    set_target_properties(vela-bluetooth-testing PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_link_libraries(vela-bluetooth-testing PRIVATE vela-bluetooth-core)
    target_compile_definitions(vela-bluetooth-testing PRIVATE VELA_BLUETOOTH_TESTING
        VELA_BT_TEST_UI="${PROJECT_BINARY_DIR}/tests/bluetooth/vela-bluetooth-daemon-test"
        VELA_BT_TEST_CLIENT="${PROJECT_BINARY_DIR}/tests/bluetooth/vela-bluetooth-client-test"
        VELA_BT_TEST_SHELL="${PROJECT_BINARY_DIR}/tests/bluetooth/vela-bluetooth-shell-test")

    # The pure parts: errors, steps, rfkill, names, answers (callers:
    # common/caller).
    find_package(GTest)
    if(GTest_FOUND)
        add_executable(vela-bluetooth-tests
            tests/describe_test.cpp
            tests/outcome_test.cpp
            tests/question_test.cpp
            tests/rfkill_test.cpp)
        target_compile_options(vela-bluetooth-tests PRIVATE -Wall -Wextra)
        target_link_libraries(vela-bluetooth-tests PRIVATE vela-bluetooth-core GTest::gtest_main)
        include(GoogleTest)
        gtest_discover_tests(vela-bluetooth-tests TEST_PREFIX "bluetooth.")
    else()
        message(STATUS "GoogleTest not found: no Bluetooth unit tests (package gtest)")
    endif()
endif()
