# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-bluetooth-client (docs/bluetooth.md §8): the shell's and Settings'
# side of vela-bluetooth, over D-Bus. Without the daemon it is just empty.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus)

add_library(vela-bluetooth-client STATIC
    bluetooth.cpp
    bluetooth.h)
set_target_properties(vela-bluetooth-client PROPERTIES AUTOMOC ON)
# requested.h: the switch shown at once (shell/src).
# Included as "bluetooth/bluetooth.h": the compositor has a bluetooth.h too.
target_include_directories(vela-bluetooth-client PUBLIC ${PROJECT_SOURCE_DIR}/common ${PROJECT_SOURCE_DIR}/shell/src)
target_compile_options(vela-bluetooth-client PRIVATE -Wall -Wextra)
target_link_libraries(vela-bluetooth-client PUBLIC Qt6::Core Qt6::DBus)

# The tests are in tests/bluetooth, next to the fake BlueZ.
