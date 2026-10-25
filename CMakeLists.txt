# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.22)

project(vela
    VERSION 0.1.0
    DESCRIPTION "A modern, fluid and sharp desktop environment for Linux"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

option(VELA_BUILD_CAPTURE_TOOLS "Build the lightweight screen-dump CLI separately from the GPU test tools" OFF)
option(VELA_BUILD_GPU_TEST_TOOLS "Build GPU-intensive test clients (only relevant when VELA_BUILD_TOOLS=ON)" ON)

option(VELA_BUILD_COMPOSITOR "Build the compositor (needs wlroots)" ON)
option(VELA_BUILD_SHELL "Build the shell, Files and Settings (needs Qt 6 and LayerShellQt)" ON)
option(VELA_BUILD_TOOLS "Build the test tools (vela-screen-dump, vela-windows, vela-input...) and the tests (ctest)" ON)
if(VELA_BUILD_TOOLS)
    enable_testing()
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME packaging-update COMMAND ${Python3_EXECUTABLE} -m unittest discover
        -s ${PROJECT_SOURCE_DIR}/packaging/tests -v)
    set_tests_properties(packaging-update PROPERTIES TIMEOUT 120)
    add_test(NAME script-tests COMMAND ${Python3_EXECUTABLE} -m unittest discover
        -s ${PROJECT_SOURCE_DIR}/scripts/tests -v)
    add_test(NAME documentation COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check-docs.py)
    add_test(NAME shared-values COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check-shared-values.py)
    # Reports only until the private controls are replaced (docs/visual-coherence.md §8).
    add_test(NAME qml-motion COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check-qml-motion.py)
    add_test(NAME functional-harness COMMAND ${Python3_EXECUTABLE} -m unittest discover
        -s ${PROJECT_SOURCE_DIR}/tests/harness -v)
    set_tests_properties(script-tests documentation shared-values qml-motion functional-harness PROPERTIES TIMEOUT 30)
    set_tests_properties(functional-harness PROPERTIES
        ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/tests/functional")
endif()

option(VELA_BUILD_REPORT "Build the standalone diagnostic reporter (needs Python 3)" ON)
if(VELA_BUILD_REPORT)
    add_subdirectory(report)
endif()

if(VELA_BUILD_COMPOSITOR)
    add_subdirectory(compositor)
    add_subdirectory(brightness)
    add_subdirectory(session)
endif()
option(VELA_BUILD_POLKIT "Build the polkit agent (needs polkit-agent-1; the prompt also needs the shell's Qt)" ON)
if(VELA_BUILD_SHELL OR VELA_BUILD_CAPTURE_TOOLS OR VELA_BUILD_TOOLS)
    add_subdirectory(capture)
endif()
if(VELA_BUILD_SHELL)
    add_subdirectory(common)
    add_subdirectory(shell)
    add_subdirectory(settings)
    add_subdirectory(explorer)
    add_subdirectory(lock)
endif()
if(VELA_BUILD_POLKIT)
    add_subdirectory(polkit)
endif()
if(VELA_BUILD_TOOLS OR VELA_BUILD_CAPTURE_TOOLS)
    add_subdirectory(tools)
endif()
if(VELA_BUILD_TOOLS)
    # The functional and sharpness tests, in sessions without a display
    # (tests/functional): they need the compositor and a GPU; without a GPU
    # they are reported as skipped (code 77).
    if(VELA_BUILD_COMPOSITOR)
        if(Python3_FOUND)
            # One entry per module (test_shell.py becomes functional-shell),
            # so each stays well within its timeout however the suite grows.
            file(GLOB VELA_FUNCTIONAL_MODULES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/functional/test_*.py)
            set(VELA_FUNCTIONAL_TESTS)
            foreach(module_path IN LISTS VELA_FUNCTIONAL_MODULES)
                get_filename_component(module ${module_path} NAME_WE)
                string(REGEX REPLACE "^test_" "" name ${module})
                string(REPLACE "_" "-" name ${name})
                add_test(NAME functional-${name}
                    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/functional/run.py ${module})
                list(APPEND VELA_FUNCTIONAL_TESTS functional-${name})
            endforeach()
            add_test(NAME sharpness COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/functional/run.py --sharpness)
            add_test(NAME sharpness-cpu COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/functional/run.py --sharpness)
            set_tests_properties(${VELA_FUNCTIONAL_TESTS} sharpness sharpness-cpu PROPERTIES
                ENVIRONMENT "VELA_BUILD=${CMAKE_BINARY_DIR}"
                SKIP_RETURN_CODE 77
                TIMEOUT 600
                LABELS gpu
                RUN_SERIAL TRUE)
            # Only isolated, state/geometry-oriented suites may overlap on a
            # single GPU. Timing, animation, rendering parity, resource stress,
            # sharpness, real Qt shell and driver-sensitive tests stay exclusive.
            # COST schedules these independent suites together before serial
            # tests, so --parallel 2 actually overlaps their execution.
            set_tests_properties(functional-windows PROPERTIES RUN_SERIAL FALSE COST 45)
            set_tests_properties(functional-outputs PROPERTIES RUN_SERIAL FALSE COST 35)
            set_tests_properties(functional-apps PROPERTIES RUN_SERIAL FALSE COST 10)
            set_property(TEST ${VELA_FUNCTIONAL_TESTS} APPEND PROPERTY LABELS functional)
            set_property(TEST sharpness-cpu APPEND PROPERTY ENVIRONMENT "VELA_SYNC_FILE=0")
        endif()
    endif()
    # A private PipeWire and WirePlumber for the audio tests (docs/audio.md).
    add_subdirectory(tests/audio)
endif()
