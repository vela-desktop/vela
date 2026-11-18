# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# Fuzz targets for the parsers that read bytes Vela doesn't control:
# vela.conf, .desktop/.ini files and desktop.conf, DDC/CI replies from
# monitors. Each target is built twice when asked:
#   - vela-fuzz-replay-<name>: the target with replay.c, any compiler; ctest
#     runs it over corpus/<name> and a few thousand seeded inputs;
#   - vela-fuzz-<name>: the target with libFuzzer (Clang,
#     -DVELA_BUILD_FUZZERS=ON), for real fuzzing runs.
set(VELA_FUZZ_TARGETS config ini ddc)
set(VELA_FUZZ_config_LIBS vela-pure)
set(VELA_FUZZ_ini_LIBS vela-pure)
set(VELA_FUZZ_ddc_LIBS vela-brightness-core)
set(VELA_FUZZ_config_FIXTURE fuzz_files.c)
set(VELA_FUZZ_ini_FIXTURE fuzz_files.c)

if(VELA_BUILD_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "VELA_BUILD_FUZZERS needs Clang (libFuzzer)")
    endif()
    set(fuzz_compile_options -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined)
    # Rebuild the parser code for libFuzzer so coverage and sanitizer checks
    # reach inside the parsers. Replay and production keep their usual libraries.
    add_library(vela-fuzz-config-parsers OBJECT
        fuzz_files.c
        ../compositor/src/config.c
        ../compositor/src/desktop_conf.c
        ../compositor/src/legacy_names.c
        ../compositor/src/path_security.c
        ../compositor/src/util.c
        ../common/alloc/alloc.c)
    target_include_directories(vela-fuzz-config-parsers PUBLIC ../compositor/src ../common)
    target_compile_definitions(vela-fuzz-config-parsers PUBLIC _GNU_SOURCE)
    target_compile_options(vela-fuzz-config-parsers PRIVATE ${fuzz_compile_options})
    set_target_properties(vela-fuzz-config-parsers PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
endif()

foreach(name IN LISTS VELA_FUZZ_TARGETS)
    if(VELA_BUILD_TOOLS)
        add_executable(vela-fuzz-replay-${name} fuzz_${name}.c ${VELA_FUZZ_${name}_FIXTURE} replay.c)
        target_include_directories(vela-fuzz-replay-${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(vela-fuzz-replay-${name} PRIVATE ${VELA_FUZZ_${name}_LIBS})
        target_compile_options(vela-fuzz-replay-${name} PRIVATE -Wall -Wextra -Werror)
        set_target_properties(vela-fuzz-replay-${name} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
        add_test(NAME fuzz-replay.${name}
            COMMAND vela-fuzz-replay-${name} ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name} --random 5000)
    endif()
    if(VELA_BUILD_FUZZERS)
        add_executable(vela-fuzz-${name} fuzz_${name}.c)
        if(name STREQUAL "ddc")
            target_sources(vela-fuzz-${name} PRIVATE ../brightness/src/ddc.c)
            target_include_directories(vela-fuzz-${name} PRIVATE ../brightness/src)
        else()
            target_link_libraries(vela-fuzz-${name} PRIVATE vela-fuzz-config-parsers)
        endif()
        target_compile_options(vela-fuzz-${name} PRIVATE ${fuzz_compile_options})
        target_link_options(vela-fuzz-${name} PRIVATE -fsanitize=fuzzer,address,undefined)
        set_target_properties(vela-fuzz-${name} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    endif()
endforeach()
