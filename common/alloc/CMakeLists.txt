# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-alloc: the host allocation helpers (alloc.h,
# docs/memory-allocation-policy.md), for the compositor, the session
# daemons and libvela-capture. C17, no dependencies.
add_library(vela-alloc STATIC alloc.c)
# Included as "alloc/alloc.h".
target_include_directories(vela-alloc PUBLIC ${PROJECT_SOURCE_DIR}/common)
target_compile_definitions(vela-alloc PRIVATE _GNU_SOURCE)
target_compile_options(vela-alloc PRIVATE -Wall -Wextra)
set_target_properties(vela-alloc PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)

if(VELA_BUILD_TOOLS)
    # The contract and the failure line. calloc and realloc are wrapped so a
    # real allocation failure can be forced.
    add_executable(vela-alloc-test tests/alloc_test.c)
    target_compile_options(vela-alloc-test PRIVATE -Wall -Wextra -Werror)
    target_link_options(vela-alloc-test PRIVATE -Wl,--wrap=calloc -Wl,--wrap=realloc)
    target_link_libraries(vela-alloc-test PRIVATE vela-alloc)
    set_target_properties(vela-alloc-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    # The failure paths run in children that abort: flush their coverage.
    if(CMAKE_C_FLAGS MATCHES "--coverage")
        target_compile_definitions(vela-alloc-test PRIVATE VELA_TEST_COVERAGE)
    endif()
    add_test(NAME alloc.helpers COMMAND vela-alloc-test)
endif()
