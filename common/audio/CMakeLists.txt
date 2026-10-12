# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-audio (docs/audio.md): the audio backend the shell and Settings
# share, on libwireplumber. GLib and WirePlumber stay inside core.cpp;
# libpipewire's streams inside levelmeter.cpp and testsound.cpp.

find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AUDIO_WIREPLUMBER REQUIRED IMPORTED_TARGET wireplumber-0.5 libpipewire-0.3)

add_library(vela-audio STATIC
    audio.cpp
    audio.h
    core.cpp
    core.h
    levelmeter.cpp
    levelmeter.h
    model.h
    testsound.cpp
    testsound.h
    volumesteps.h)
set_target_properties(vela-audio PROPERTIES AUTOMOC ON)
target_include_directories(vela-audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(vela-audio PRIVATE -Wall -Wextra)
target_link_libraries(vela-audio PUBLIC Qt6::Core PRIVATE Qt6::DBus PkgConfig::AUDIO_WIREPLUMBER)

# The tests are in tests/audio, next to their fixture.
