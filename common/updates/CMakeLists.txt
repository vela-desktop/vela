# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-updates-client (docs/updates.md §8): the shell's and Settings' side
# of vela-update-daemon, over D-Bus. Without the daemon it is just empty.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus)

add_library(vela-updates-client STATIC
    updates.cpp
    updates.h)
set_target_properties(vela-updates-client PROPERTIES AUTOMOC ON)
# requested.h: the switch shown at once (shell/src).
target_include_directories(vela-updates-client PUBLIC ${PROJECT_SOURCE_DIR}/common ${PROJECT_SOURCE_DIR}/shell/src)
target_compile_options(vela-updates-client PRIVATE -Wall -Wextra)
target_link_libraries(vela-updates-client PUBLIC Qt6::Core Qt6::DBus)

# The tests are in tests/updates, next to the fake vela-update.
