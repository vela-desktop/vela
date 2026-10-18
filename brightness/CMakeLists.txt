# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-brightness, the helper that drives one output's brightness
# (docs/brightness.md §5). C17 on the C library alone: its own DDC/CI over
# /dev/i2c-N, no libddcutil.
add_library(vela-brightness-core STATIC
    src/backlight.c
    src/ddc.c
    src/device.c
    src/fake.c
    src/helper.c
    src/i2c.c
    src/lines.c
    src/probe.c
    src/scheduler.c
    src/transfer.c)
target_include_directories(vela-brightness-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(vela-brightness-core PUBLIC _GNU_SOURCE)
target_compile_options(vela-brightness-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(vela-brightness-core PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
# The built-in backlight is written through logind (§5.3), with sd-bus.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(SYSTEMD IMPORTED_TARGET libsystemd)
endif()
if(SYSTEMD_FOUND)
    target_compile_definitions(vela-brightness-core PRIVATE VELA_HAVE_SD_BUS)
    target_link_libraries(vela-brightness-core PUBLIC PkgConfig::SYSTEMD)
else()
    message(STATUS "libsystemd not found: vela-brightness can't change a laptop's backlight")
endif()

add_executable(vela-brightness src/main.c)
target_compile_options(vela-brightness PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(vela-brightness PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(vela-brightness PRIVATE vela-brightness-core)
install(TARGETS vela-brightness RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
# The session's user reaches the displays' I2C buses, and i2c-dev is loaded
# at boot (§13). Under the prefix's lib/, not libdir: udev and systemd look
# in /usr/lib on every distribution.
install(FILES 70-vela-i2c.rules DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/udev/rules.d)
install(FILES vela-i2c.conf DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/modules-load.d)

# Protocol, scheduler, fake monitors and the helper's lines, on a fake clock:
# no hardware, no waiting (§12).
if(VELA_BUILD_TOOLS)
    find_package(GTest)
    if(GTest_FOUND)
        add_executable(vela-brightness-tests
            tests/backlight_test.cpp
            tests/ddc_test.cpp
            tests/fake_test.cpp
            tests/helper_test.cpp
            tests/i2c_test.cpp
            tests/lines_test.cpp
            tests/scheduler_test.cpp)
        target_compile_options(vela-brightness-tests PRIVATE -Wall -Wextra)
        target_link_libraries(vela-brightness-tests PRIVATE vela-brightness-core GTest::gtest_main)
        include(GoogleTest)
        gtest_discover_tests(vela-brightness-tests TEST_PREFIX "brightness.")
    else()
        message(STATUS "GoogleTest not found: no brightness tests (package gtest)")
    endif()
    # --probe on the fake monitors, as a bug report would run it.
    add_test(NAME brightness.probe-fake COMMAND vela-brightness --probe)
    set_tests_properties(brightness.probe-fake PROPERTIES
        ENVIRONMENT "VELA_BRIGHTNESS_FAKE=${CMAKE_CURRENT_SOURCE_DIR}/tests/monitors.conf"
        PASS_REGULAR_EXPRESSION "HDMI-A-1: fake monitor.*result: ok 100/100.*DP-3: fake monitor.*result: ok 100/100.*OFF-1: fake monitor.*result: no-answer.*NOBUS-1: no bus"
        TIMEOUT 30)
endif()
