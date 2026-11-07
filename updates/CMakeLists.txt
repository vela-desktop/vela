# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-update-daemon, the session daemon behind Settings > Updates and the
# update notification (docs/updates.md §6). C17 with sd-bus, sd-event and
# sd-json (libsystemd 257 or later): no Qt, no GLib.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(UPDATES_SYSTEMD IMPORTED_TARGET libsystemd>=257)
endif()
if(NOT UPDATES_SYSTEMD_FOUND OR NOT TARGET vela-caller)
    message(STATUS "libsystemd 257 or later not found: no vela-update-daemon")
    return()
endif()

add_library(vela-update-core STATIC
    src/check.c
    src/child.c
    src/install.c
    src/report.c
    src/service.c
    src/state.c)
target_include_directories(vela-update-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(vela-update-core PUBLIC _GNU_SOURCE)
target_compile_options(vela-update-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(vela-update-core PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
# vela.conf (update-check) is read and written as the compositor does.
target_link_libraries(vela-update-core PUBLIC vela-pure vela-caller PkgConfig::UPDATES_SYSTEMD)

# vela-update: installed in bindir; a daemon run from its build directory
# uses the source tree's.
set(UPDATE_DEFINITIONS
    VELA_UP_COMMAND="${CMAKE_INSTALL_FULL_BINDIR}/vela-update"
    VELA_UP_SOURCE_COMMAND="${PROJECT_SOURCE_DIR}/packaging/vela-update"
    VELA_UP_BUILD_SELF="${CMAKE_CURRENT_BINARY_DIR}/vela-update-daemon")
add_executable(vela-update-daemon src/main.c)
target_compile_options(vela-update-daemon PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(vela-update-daemon PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(vela-update-daemon PRIVATE vela-update-core)
target_compile_definitions(vela-update-daemon PRIVATE ${UPDATE_DEFINITIONS})
install(TARGETS vela-update-daemon RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

if(VELA_BUILD_TOOLS)
    # The daemon the tests run (§10): a fake vela-update and terminal, and
    # timings in milliseconds. Never installed.
    add_executable(vela-update-daemon-testing src/main.c)
    target_compile_options(vela-update-daemon-testing PRIVATE -Wall -Wextra -Wno-unused-parameter)
    set_target_properties(vela-update-daemon-testing PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_link_libraries(vela-update-daemon-testing PRIVATE vela-update-core)
    target_compile_definitions(vela-update-daemon-testing PRIVATE ${UPDATE_DEFINITIONS} VELA_UPDATE_TESTING)

    # The pure parts: reports, options, the terminal, the session process.
    find_package(GTest)
    if(GTest_FOUND)
        add_executable(vela-update-tests tests/report_test.cpp)
        target_compile_options(vela-update-tests PRIVATE -Wall -Wextra)
        target_link_libraries(vela-update-tests PRIVATE vela-update-core GTest::gtest_main)
        include(GoogleTest)
        gtest_discover_tests(vela-update-tests TEST_PREFIX "updates.")
    else()
        message(STATUS "GoogleTest not found: no update daemon unit tests (package gtest)")
    endif()
endif()
