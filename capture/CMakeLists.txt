# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# The capture engine is C17 and does not link Qt, wlroots, Vulkan or a PNG library.
find_package(PkgConfig REQUIRED)
pkg_check_modules(CAPTURE_WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(CAPTURE_PROTOCOLS REQUIRED wayland-protocols>=1.37)
pkg_get_variable(CAPTURE_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(CAPTURE_WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

set(CAPTURE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${CAPTURE_GENERATED_DIR}")
set(CAPTURE_GENERATED_FILES)
foreach(name
        ext-image-capture-source/ext-image-capture-source-v1
        ext-image-copy-capture/ext-image-copy-capture-v1)
    get_filename_component(base "${name}" NAME)
    set(xml "${CAPTURE_PROTOCOLS_DIR}/staging/${name}.xml")
    set(header "${CAPTURE_GENERATED_DIR}/${base}-client-protocol.h")
    set(code "${CAPTURE_GENERATED_DIR}/${base}-protocol.c")
    add_custom_command(
        OUTPUT "${header}" "${code}"
        COMMAND "${CAPTURE_WAYLAND_SCANNER}" client-header "${xml}" "${header}"
        COMMAND "${CAPTURE_WAYLAND_SCANNER}" private-code "${xml}" "${code}"
        DEPENDS "${xml}"
        COMMENT "Generating shared ${base} Wayland client protocol")
    list(APPEND CAPTURE_GENERATED_FILES "${header}" "${code}")
endforeach()

add_library(vela-capture STATIC src/capture.c ${CAPTURE_GENERATED_FILES})
target_include_directories(vela-capture PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CAPTURE_GENERATED_DIR}")
target_link_libraries(vela-capture PUBLIC PkgConfig::CAPTURE_WAYLAND)
target_compile_definitions(vela-capture PRIVATE _GNU_SOURCE)
set_target_properties(vela-capture PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(vela-capture PRIVATE -Wall -Wextra -Wno-unused-parameter)
