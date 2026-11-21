# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-terminal-lookup: the terminal of docs/keyboard.md §11 (terminal.h),
# for vela-terminal (Win+Enter), the updater and Settings. C17, common/alloc
# only.
add_library(vela-terminal-lookup STATIC terminal.c)
target_include_directories(vela-terminal-lookup PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(vela-terminal-lookup PRIVATE _GNU_SOURCE)
target_compile_options(vela-terminal-lookup PRIVATE -Wall -Wextra)
target_link_libraries(vela-terminal-lookup PUBLIC vela-alloc)
set_target_properties(vela-terminal-lookup PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)

add_executable(vela-terminal vela-terminal.c)
target_compile_options(vela-terminal PRIVATE -Wall -Wextra)
target_link_libraries(vela-terminal PRIVATE vela-terminal-lookup)
set_target_properties(vela-terminal PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
install(TARGETS vela-terminal RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

if(VELA_BUILD_TOOLS)
    find_package(GTest)
    if(GTest_FOUND)
        add_executable(vela-terminal-tests tests/terminal_test.cpp)
        target_compile_options(vela-terminal-tests PRIVATE -Wall -Wextra)
        target_link_libraries(vela-terminal-tests PRIVATE vela-terminal-lookup GTest::gtest_main)
        include(GoogleTest)
        gtest_discover_tests(vela-terminal-tests TEST_PREFIX "terminal."
            PROPERTIES ENVIRONMENT "VELA_TERMINAL_PROGRAM=$<TARGET_FILE:vela-terminal>")
    endif()
endif()
