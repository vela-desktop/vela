# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-caller: who may call a Vela session daemon on D-Bus (caller.h),
# shared by vela-bluetooth and vela-update-daemon. C17 with sd-bus.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(CALLER_SYSTEMD IMPORTED_TARGET libsystemd)
endif()
if(NOT CALLER_SYSTEMD_FOUND)
    return()
endif()

add_library(vela-caller STATIC caller.c)
# Included as "caller/caller.h".
target_include_directories(vela-caller PUBLIC ${PROJECT_SOURCE_DIR}/common PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(vela-caller PRIVATE _GNU_SOURCE)
target_compile_options(vela-caller PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(vela-caller PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(vela-caller PUBLIC PkgConfig::CALLER_SYSTEMD)
# The caller's pidfd from the bus (systemd 256 and later): a PID could be
# reused while it's checked.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${CALLER_SYSTEMD_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${CALLER_SYSTEMD_LINK_LIBRARIES})
check_c_source_compiles("
#include <systemd/sd-bus.h>
int main(void) { int fd; return sd_bus_creds_get_pidfd_dup(0, &fd) + (int)SD_BUS_CREDS_PIDFD; }"
    VELA_HAVE_PIDFD_CREDS)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
if(VELA_HAVE_PIDFD_CREDS)
    target_compile_definitions(vela-caller PRIVATE VELA_HAVE_PIDFD_CREDS)
endif()

if(VELA_BUILD_TOOLS)
    # Sandboxes on a fake /proc, Vela's executables, a pidfd's process.
    find_package(GTest)
    if(GTest_FOUND)
        add_executable(vela-caller-tests tests/caller_test.cpp)
        target_compile_options(vela-caller-tests PRIVATE -Wall -Wextra)
        target_include_directories(vela-caller-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(vela-caller-tests PRIVATE vela-caller GTest::gtest_main)
        include(GoogleTest)
        gtest_discover_tests(vela-caller-tests TEST_PREFIX "caller.")
    endif()
endif()
