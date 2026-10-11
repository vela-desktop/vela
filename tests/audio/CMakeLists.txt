# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# The audio tests' fixture (docs/audio.md §11): a private PipeWire and
# WirePlumber with fake devices, and a fake app. Without libpipewire there
# is no fixture; without the pipewire and wireplumber programs the tests
# are reported as skipped (code 77).

find_package(PkgConfig REQUIRED)
pkg_check_modules(AUDIO_TEST_PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
if(NOT AUDIO_TEST_PIPEWIRE_FOUND)
    message(STATUS "libpipewire-0.3 not found: no audio test fixture")
    return()
endif()

add_executable(vela-audio-fake-stream fake-stream.c)
target_compile_options(vela-audio-fake-stream PRIVATE -Wall -Wextra)
target_link_libraries(vela-audio-fake-stream PRIVATE PkgConfig::AUDIO_TEST_PIPEWIRE m)

add_test(NAME audio-fixture
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_fixture.py)
set_tests_properties(audio-fixture PROPERTIES
    ENVIRONMENT "VELA_AUDIO_FAKE_STREAM=$<TARGET_FILE:vela-audio-fake-stream>;PYTHONDONTWRITEBYTECODE=1"
    SKIP_RETURN_CODE 77
    TIMEOUT 120)

# vela-audio's tests (common/audio), each run inside the fixture.
if(TARGET vela-audio)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Network Test)
    foreach(suite core view)
        qt_add_executable(vela-audio-${suite}-test ${suite}.cpp harness.h)
        set_target_properties(vela-audio-${suite}-test PROPERTIES AUTOMOC ON)
        target_compile_options(vela-audio-${suite}-test PRIVATE -Wall -Wextra)
        target_link_libraries(vela-audio-${suite}-test PRIVATE vela-audio Qt6::Network Qt6::Test)
        add_test(NAME audio-${suite}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fixture.py
                -- $<TARGET_FILE:vela-audio-${suite}-test>)
        set_tests_properties(audio-${suite} PROPERTIES
            ENVIRONMENT "VELA_AUDIO_FAKE_STREAM=$<TARGET_FILE:vela-audio-fake-stream>;PYTHONDONTWRITEBYTECODE=1"
            SKIP_RETURN_CODE 77
            TIMEOUT 180)
    endforeach()
endif()
