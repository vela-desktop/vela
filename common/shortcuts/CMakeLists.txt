# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-shortcuts: the key combinations, their matcher and Vela's shortcut
# table (docs/keyboard.md §9.2), shared by the compositor and Settings so
# that what Settings accepts is what the compositor runs. C17, xkbcommon.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SHORTCUTS_XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
add_library(vela-shortcuts STATIC custom.c shortcuts.c super_numbers.c table.c)
target_include_directories(vela-shortcuts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(vela-shortcuts PRIVATE -Wall -Wextra)
target_link_libraries(vela-shortcuts PUBLIC PkgConfig::SHORTCUTS_XKBCOMMON)
set_target_properties(vela-shortcuts PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
