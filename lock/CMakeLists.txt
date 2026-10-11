# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# vela-lock: the lock screen (ext-session-lock-v1 + PAM).

find_package(Qt6 6.5 REQUIRED COMPONENTS Gui GuiPrivate Svg)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LOCK_DEPS REQUIRED IMPORTED_TARGET wayland-client xkbcommon)
pkg_get_variable(LOCK_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
find_library(PAM_LIBRARY pam REQUIRED)

set(LOCK_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${LOCK_GENERATED_DIR})
set(LOCK_PROTOCOL_SOURCES)
foreach(xml
        staging/ext-session-lock/ext-session-lock-v1.xml
        staging/fractional-scale/fractional-scale-v1.xml
        stable/viewporter/viewporter.xml)
    get_filename_component(name ${xml} NAME_WE)
    set(header ${LOCK_GENERATED_DIR}/${name}-client-protocol.h)
    set(code ${LOCK_GENERATED_DIR}/${name}-protocol.c)
    add_custom_command(
        OUTPUT ${header} ${code}
        COMMAND ${WAYLAND_SCANNER} client-header ${LOCK_WAYLAND_PROTOCOLS_DIR}/${xml} ${header}
        COMMAND ${WAYLAND_SCANNER} private-code ${LOCK_WAYLAND_PROTOCOLS_DIR}/${xml} ${code}
        DEPENDS ${LOCK_WAYLAND_PROTOCOLS_DIR}/${xml}
        COMMENT "Generating the ${name} client")
    list(APPEND LOCK_PROTOCOL_SOURCES ${header} ${code})
endforeach()

qt_add_executable(vela-lock main.cpp ${LOCK_PROTOCOL_SOURCES})
# The shell's default wallpaper.
qt_add_resources(vela-lock "images"
    PREFIX "/vela"
    BASE ${PROJECT_SOURCE_DIR}
    FILES ${PROJECT_SOURCE_DIR}/images/vela_splash_169.svg)
target_include_directories(vela-lock PRIVATE ${LOCK_GENERATED_DIR} ${PROJECT_SOURCE_DIR}/shell/src)
target_compile_options(vela-lock PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(vela-lock PRIVATE Qt6::Gui Qt6::Svg PkgConfig::LOCK_DEPS ${PAM_LIBRARY} vela-app)

install(TARGETS vela-lock RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The PAM service: the same check as a console login.
install(FILES vela-lock.pam DESTINATION /etc/pam.d RENAME vela-lock)

# Italian translation: strings in the code are in English, Italian lives
# in i18n/vela-lock_it.ts and ends up in the executable (:/i18n/vela-lock_it.qm, see
# shell/src/language.h). After changing texts:
#   cmake --build build --target update_translations
find_package(Qt6 6.7 REQUIRED COMPONENTS LinguistTools)
qt_add_translations(TARGETS vela-lock SOURCE_TARGETS vela-lock
    TS_FILES i18n/vela-lock_it.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_TARGET vela-lock_lupdate
    LRELEASE_TARGET vela-lock_lrelease
    LUPDATE_OPTIONS -source-language en -target-language it -no-obsolete -locations none)
