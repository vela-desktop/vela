# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# Vela's polkit agent (docs/polkit-agent.md): vela-polkit-agent (GLib and
# libpolkit-agent-1, always running, no interface) and vela-polkit-prompt
# (Qt Quick, one per request). They sit in the same directory in the build
# too: the agent looks for the prompt next to itself.

find_package(PkgConfig REQUIRED)
pkg_check_modules(POLKIT_AGENT_DEPS REQUIRED IMPORTED_TARGET polkit-agent-1 gio-unix-2.0)

# The core, without GLib: the agent and the tests use it.
add_library(vela-polkit-core STATIC agent/agent.cpp agent/agent.hpp protocol.hpp)
target_compile_options(vela-polkit-core PRIVATE -Wall -Wextra)

add_executable(vela-polkit-agent
    agent/main.cpp
    agent/backend.cpp
    agent/backend.hpp
    agent/system.cpp
    agent/system.hpp)
target_compile_options(vela-polkit-agent PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_compile_definitions(vela-polkit-agent PRIVATE VELA_LIBEXECDIR="${CMAKE_INSTALL_FULL_LIBEXECDIR}")
target_link_libraries(vela-polkit-agent PRIVATE vela-alloc vela-polkit-core PkgConfig::POLKIT_AGENT_DEPS)
install(TARGETS vela-polkit-agent RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

if(VELA_BUILD_SHELL)
    add_subdirectory(prompt)
endif()

# The core's tests (GoogleTest, like the compositor's): queue, cancellations,
# identities, protocol. No polkit, no PAM.
if(VELA_BUILD_TOOLS)
    find_package(GTest)
    if(GTest_FOUND)
        include(GoogleTest)
        add_executable(vela-polkit-tests tests/agent_test.cpp)
        target_compile_options(vela-polkit-tests PRIVATE -Wall -Wextra)
        target_link_libraries(vela-polkit-tests PRIVATE vela-polkit-core GTest::gtest_main)
        gtest_discover_tests(vela-polkit-tests TEST_PREFIX "polkit.")
    else()
        message(STATUS "GoogleTest not found: no polkit agent tests (package gtest)")
    endif()
endif()
