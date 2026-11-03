# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# Tools to test Vela hands-off: screenshots, listing/commanding windows,
# simulated mouse and keyboard. They speak only standard Wayland protocols.

find_package(PkgConfig REQUIRED)
pkg_check_modules(TOOLS_WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
if(VELA_BUILD_TOOLS)
    pkg_check_modules(TOOLS_XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
endif()
pkg_check_modules(TOOLS_ZLIB REQUIRED IMPORTED_TARGET zlib)
pkg_check_modules(TOOLS_WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(TOOLS_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

set(TOOLS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${TOOLS_GENERATED_DIR})

# A protocol's header and client code, in a static library.
function(vela_client_protocol target xml name)
    set(header ${TOOLS_GENERATED_DIR}/${name}-client-protocol.h)
    set(code ${TOOLS_GENERATED_DIR}/${name}-protocol.c)
    add_custom_command(
        OUTPUT ${header} ${code}
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${header}
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${code}
        DEPENDS ${xml}
        COMMENT "Generating the ${name} client")
    add_library(${target} STATIC ${code} ${header})
    target_include_directories(${target} PUBLIC ${TOOLS_GENERATED_DIR})
    target_link_libraries(${target} PUBLIC PkgConfig::TOOLS_WAYLAND_CLIENT)
endfunction()

set(PROTOCOLS ${PROJECT_SOURCE_DIR}/protocols)

add_executable(vela-screen-dump vela-screen-dump.c)
target_link_libraries(vela-screen-dump PRIVATE vela-capture PkgConfig::TOOLS_ZLIB)
target_compile_options(vela-screen-dump PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Frame-by-frame recordings for videos (scripts/record-before-after.py).
add_executable(vela-screen-record vela-screen-record.c)
target_link_libraries(vela-screen-record PRIVATE vela-capture)
target_compile_options(vela-screen-record PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(VELA_BUILD_TOOLS)

add_executable(vela-capture-probe vela-capture-probe.c)
target_link_libraries(vela-capture-probe PRIVATE vela-capture)
target_compile_options(vela-capture-probe PRIVATE -Wall -Wextra -Wno-unused-parameter)

vela_client_protocol(proto-foreign-toplevel ${PROTOCOLS}/wlr-foreign-toplevel-management-unstable-v1.xml
    wlr-foreign-toplevel-management-unstable-v1)
vela_client_protocol(proto-virtual-pointer ${PROTOCOLS}/wlr-virtual-pointer-unstable-v1.xml
    wlr-virtual-pointer-unstable-v1)
vela_client_protocol(proto-virtual-keyboard ${PROTOCOLS}/virtual-keyboard-unstable-v1.xml
    virtual-keyboard-unstable-v1)
vela_client_protocol(proto-xdg-output
    ${TOOLS_WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml
    xdg-output-unstable-v1)

vela_client_protocol(proto-xdg-shell ${TOOLS_WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml xdg-shell)
vela_client_protocol(proto-presentation-time
    ${TOOLS_WAYLAND_PROTOCOLS_DIR}/stable/presentation-time/presentation-time.xml presentation-time)
add_executable(vela-bench-probe vela-bench-probe.c)
target_link_libraries(vela-bench-probe PRIVATE proto-xdg-shell proto-presentation-time)
target_compile_options(vela-bench-probe PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_executable(vela-bench-kwin-input-fixture ../benchmarks/tests/kwin_input_fixture.c)
target_link_libraries(vela-bench-kwin-input-fixture PRIVATE PkgConfig::TOOLS_WAYLAND_CLIENT)
target_compile_options(vela-bench-kwin-input-fixture PRIVATE -Wall -Wextra)
add_executable(vela-bench-cpuload vela-bench-cpuload.c)
target_compile_options(vela-bench-cpuload PRIVATE -Wall -Wextra)
add_test(NAME benchmark-contention-cli COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/benchmarks/tests/contention_cli.py $<TARGET_FILE:vela-bench-cpuload>)
set_tests_properties(benchmark-contention-cli PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
pkg_check_modules(BENCH_EVDEV IMPORTED_TARGET libevdev)
if(BENCH_EVDEV_FOUND)
    add_executable(vela-bench-input vela-bench-input.c)
    target_link_libraries(vela-bench-input PRIVATE PkgConfig::BENCH_EVDEV)
    target_compile_options(vela-bench-input PRIVATE -Wall -Wextra)
    add_executable(vela-bench-input-fixture vela-bench-input.c ../benchmarks/tests/input_fixture.c)
    target_link_libraries(vela-bench-input-fixture PRIVATE PkgConfig::BENCH_EVDEV)
    target_compile_options(vela-bench-input-fixture PRIVATE -Wall -Wextra -Wno-unused-parameter)
    foreach(symbol getuid geteuid libevdev_uinput_create_from_device libevdev_uinput_write_event
                   libevdev_uinput_get_syspath libevdev_uinput_destroy)
        target_link_options(vela-bench-input-fixture PRIVATE "LINKER:--wrap=${symbol}")
    endforeach()
    add_test(NAME benchmark-input-cli COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/benchmarks/tests/input_cli.py $<TARGET_FILE:vela-bench-input-fixture>)
    set_tests_properties(benchmark-input-cli PROPERTIES TIMEOUT 15)
endif()
find_package(KF6Screen QUIET)
find_package(Qt6 QUIET COMPONENTS Core Gui DBus)
add_custom_target(vela-bench-corpus)
find_package(Qt6 QUIET COMPONENTS Widgets OPTIONAL_COMPONENTS WebEngineWidgets)
pkg_check_modules(BENCH_FONTCONFIG IMPORTED_TARGET fontconfig)
pkg_check_modules(BENCH_XCB IMPORTED_TARGET xcb)
pkg_check_modules(BENCH_GTK IMPORTED_TARGET gtk4>=4.12)
if(TARGET Qt6::Widgets AND BENCH_FONTCONFIG_FOUND AND BENCH_XCB_FOUND)
    add_executable(vela-bench-corpus-qt vela-bench-corpus-qt.cpp)
    target_link_libraries(vela-bench-corpus-qt PRIVATE Qt6::Widgets PkgConfig::BENCH_FONTCONFIG PkgConfig::BENCH_XCB)
    target_compile_options(vela-bench-corpus-qt PRIVATE -Wall -Wextra)
    add_dependencies(vela-bench-corpus vela-bench-corpus-qt)
    if(TARGET Qt6::WebEngineWidgets)
        add_executable(vela-bench-corpus-web vela-bench-corpus-qt.cpp)
        target_compile_definitions(vela-bench-corpus-web PRIVATE CORPUS_WEB=1)
        target_link_libraries(vela-bench-corpus-web PRIVATE Qt6::WebEngineWidgets PkgConfig::BENCH_FONTCONFIG PkgConfig::BENCH_XCB)
        target_compile_options(vela-bench-corpus-web PRIVATE -Wall -Wextra)
        add_dependencies(vela-bench-corpus vela-bench-corpus-web)
    endif()
endif()
if(BENCH_GTK_FOUND AND BENCH_FONTCONFIG_FOUND)
    add_executable(vela-bench-corpus-gtk vela-bench-corpus-gtk.c)
    target_link_libraries(vela-bench-corpus-gtk PRIVATE PkgConfig::BENCH_GTK PkgConfig::BENCH_FONTCONFIG)
    target_compile_options(vela-bench-corpus-gtk PRIVATE -Wall -Wextra)
    add_dependencies(vela-bench-corpus vela-bench-corpus-gtk)
endif()
foreach(corpus_tool vela-bench-corpus-qt vela-bench-corpus-gtk vela-bench-corpus-web)
    if(TARGET ${corpus_tool})
        add_test(NAME benchmark-${corpus_tool}-cli COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/benchmarks/tests/corpus_cli.py $<TARGET_FILE:${corpus_tool}>)
        set_tests_properties(benchmark-${corpus_tool}-cli PROPERTIES TIMEOUT 30)
    endif()
endforeach()
if(KF6Screen_FOUND AND TARGET Qt6::DBus)
    find_program(BENCH_XWAYLAND NAMES Xwayland)
    if(VELA_BUILD_COMPOSITOR)
        if(TARGET vela-bench-corpus-qt AND TARGET vela-bench-corpus-gtk AND TARGET vela-bench-corpus-web AND BENCH_XWAYLAND)
            add_test(NAME benchmark-corpus-integration COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/benchmarks/tests/p4_integration.py)
            set_tests_properties(benchmark-corpus-integration PROPERTIES
                TIMEOUT 550 LABELS "gpu" RUN_SERIAL TRUE
                ENVIRONMENT "VELA_BUILD=${CMAKE_BINARY_DIR};VELA_TEST_CORPUS=full")
        endif()
        add_test(NAME benchmark-p4-integration COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/benchmarks/tests/p4_integration.py)
        set_tests_properties(benchmark-p4-integration PROPERTIES
            TIMEOUT 180 LABELS "gpu" RUN_SERIAL TRUE ENVIRONMENT "VELA_BUILD=${CMAKE_BINARY_DIR}")
        add_test(NAME benchmark-resource-interaction-integration COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/benchmarks/tests/interaction_integration.py)
        set_tests_properties(benchmark-resource-interaction-integration PROPERTIES
            TIMEOUT 90 LABELS "gpu" RUN_SERIAL TRUE ENVIRONMENT "VELA_BUILD=${CMAKE_BINARY_DIR}")
    endif()
    add_executable(vela-bench-scene vela-bench-scene.cpp)
    target_link_libraries(vela-bench-scene PRIVATE Qt6::Gui)
    target_compile_options(vela-bench-scene PRIVATE -Wall -Wextra)
    add_executable(vela-bench-desktop vela-bench-desktop.cpp)
    target_link_libraries(vela-bench-desktop PRIVATE KF6::Screen Qt6::Gui Qt6::DBus)
    target_compile_options(vela-bench-desktop PRIVATE -Wall -Wextra)
    find_program(BENCH_KWIN NAMES kwin_wayland)
    find_program(BENCH_DBUS_RUN NAMES dbus-run-session)
    if(BENCH_DBUS_RUN AND VELA_BUILD_COMPOSITOR)
        foreach(phase motion responsiveness)
            add_test(NAME benchmark-p3-${phase}-integration COMMAND ${BENCH_DBUS_RUN} -- ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/benchmarks/tests/desktop_integration.py ${phase})
            set_tests_properties(benchmark-p3-${phase}-integration PROPERTIES TIMEOUT 900 LABELS "gpu"
                RUN_SERIAL TRUE SKIP_RETURN_CODE 77 ENVIRONMENT "VELA_BUILD=${CMAKE_BINARY_DIR}")
        endforeach()
    endif()
    if(BENCH_KWIN AND BENCH_DBUS_RUN)
        if(TARGET vela-bench-corpus-qt AND TARGET vela-bench-corpus-gtk AND TARGET vela-bench-corpus-web AND BENCH_XWAYLAND)
            add_test(NAME benchmark-plasma-corpus-integration COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/benchmarks/tests/plasma_scaling_integration.py $<TARGET_FILE:vela-bench-desktop> --corpus)
            set_tests_properties(benchmark-plasma-corpus-integration PROPERTIES
                TIMEOUT 550 LABELS "gpu" RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
        endif()
        add_test(NAME benchmark-plasma-p3-motion-integration COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/benchmarks/tests/desktop_integration.py --plasma motion)
        set_tests_properties(benchmark-plasma-p3-motion-integration PROPERTIES TIMEOUT 900 LABELS "gpu"
            RUN_SERIAL TRUE SKIP_RETURN_CODE 77 ENVIRONMENT "VELA_BUILD=${CMAKE_BINARY_DIR}")
        add_test(NAME benchmark-plasma-integration COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/benchmarks/tests/plasma_integration.py $<TARGET_FILE:vela-bench-desktop>)
        set_tests_properties(benchmark-plasma-integration PROPERTIES TIMEOUT 45 LABELS "gpu")
        add_test(NAME benchmark-plasma-scaling-integration COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/benchmarks/tests/plasma_scaling_integration.py $<TARGET_FILE:vela-bench-desktop>)
        set_tests_properties(benchmark-plasma-scaling-integration PROPERTIES TIMEOUT 180
            LABELS "gpu" RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
        add_test(NAME benchmark-plasma-resource-interaction-integration COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/benchmarks/tests/plasma_scaling_integration.py $<TARGET_FILE:vela-bench-desktop> --interactions)
        set_tests_properties(benchmark-plasma-resource-interaction-integration PROPERTIES TIMEOUT 90
            LABELS "gpu" RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
    endif()
endif()
add_test(NAME benchmark-probe-cli COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/benchmarks/tests/probe_cli.py $<TARGET_FILE:vela-bench-probe>)
set_tests_properties(benchmark-probe-cli PROPERTIES TIMEOUT 15)
vela_client_protocol(proto-viewporter ${TOOLS_WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml viewporter)
vela_client_protocol(proto-fractional-scale
    ${TOOLS_WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml fractional-scale-v1)
vela_client_protocol(proto-xdg-decoration
    ${TOOLS_WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
    xdg-decoration-unstable-v1)

add_executable(vela-windows vela-windows.c)
target_link_libraries(vela-windows PRIVATE proto-foreign-toplevel)

add_executable(vela-input vela-input.c)
target_link_libraries(vela-input PRIVATE
    proto-virtual-pointer proto-virtual-keyboard proto-xdg-output PkgConfig::TOOLS_XKBCOMMON)

vela_client_protocol(proto-session-lock
    ${TOOLS_WAYLAND_PROTOCOLS_DIR}/staging/ext-session-lock/ext-session-lock-v1.xml ext-session-lock-v1)

# Test lock program: locks and unlocks by itself, without a password.
add_executable(vela-testlock vela-testlock.c)
target_link_libraries(vela-testlock PRIVATE proto-session-lock)

# Sharpness test client (scripts/test-sharpness.sh).
vela_client_protocol(proto-pointer-constraints
    ${TOOLS_WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
    pointer-constraints-unstable-v1)
add_executable(vela-pattern vela-pattern.c)
target_link_libraries(vela-pattern PRIVATE proto-xdg-shell proto-viewporter proto-fractional-scale
    proto-xdg-decoration proto-pointer-constraints)

# Fake shell pieces (wallpaper, blurred taskbar) for tests without the Qt
# shell.
vela_client_protocol(proto-layer-shell ${PROTOCOLS}/wlr-layer-shell-unstable-v1.xml wlr-layer-shell-unstable-v1)
vela_client_protocol(proto-background-effect
    ${TOOLS_WAYLAND_PROTOCOLS_DIR}/staging/ext-background-effect/ext-background-effect-v1.xml ext-background-effect-v1)
add_executable(vela-panel vela-panel.c)
target_link_libraries(vela-panel PRIVATE proto-layer-shell proto-background-effect proto-xdg-shell)

# Output configuration like Settings > Display, without wlr-randr.
vela_client_protocol(proto-output-management ${PROTOCOLS}/wlr-output-management-unstable-v1.xml
    wlr-output-management-unstable-v1)
add_executable(vela-randr vela-randr.c)
target_link_libraries(vela-randr PRIVATE proto-output-management)

# An X11 app (through Xwayland) for the X11 window tests.
pkg_check_modules(TOOLS_XCB IMPORTED_TARGET xcb)
if(TOOLS_XCB_FOUND)
    add_executable(vela-x11 vela-x11.c)
    target_link_libraries(vela-x11 PRIVATE PkgConfig::TOOLS_XCB)
    target_compile_options(vela-x11 PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
# An X11 app that shows its frames with Present, like games, for the flip test.
pkg_check_modules(TOOLS_XCB_PRESENT IMPORTED_TARGET xcb xcb-present xcb-xfixes)
if(TOOLS_XCB_PRESENT_FOUND)
    add_executable(vela-x11-present vela-x11-present.c)
    target_link_libraries(vela-x11-present PRIVATE PkgConfig::TOOLS_XCB_PRESENT)
    target_compile_options(vela-x11-present PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# An app with a slow GPU (tests/functional/test_ready.py): a Vulkan compute job
# stands in for rendering and its fence comes late.
if(VELA_BUILD_GPU_TEST_TOOLS)
pkg_check_modules(TOOLS_VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(TOOLS_GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(TOOLS_DRM REQUIRED IMPORTED_TARGET libdrm)
find_program(TOOLS_GLSLC NAMES glslc REQUIRED)
vela_client_protocol(proto-linux-dmabuf
    ${TOOLS_WAYLAND_PROTOCOLS_DIR}/stable/linux-dmabuf/linux-dmabuf-v1.xml linux-dmabuf-v1)
vela_client_protocol(proto-linux-drm-syncobj
    ${TOOLS_WAYLAND_PROTOCOLS_DIR}/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml linux-drm-syncobj-v1)
set(TOOLS_SHADER ${CMAKE_CURRENT_BINARY_DIR}/shaders/slow.comp.spv.inc)
add_custom_command(
    OUTPUT ${TOOLS_SHADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
    COMMAND ${TOOLS_GLSLC} --target-env=vulkan1.3 -O -mfmt=num -o ${TOOLS_SHADER}
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/slow.comp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/slow.comp
    COMMENT "Compilo lo shader di vela-slowgpu")
set(CONTENTION_SHADER ${CMAKE_CURRENT_BINARY_DIR}/shaders/contention.comp.spv.inc)
add_custom_command(OUTPUT ${CONTENTION_SHADER}
    COMMAND ${TOOLS_GLSLC} --target-env=vulkan1.3 -O -mfmt=num -o ${CONTENTION_SHADER}
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/contention.comp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/contention.comp ${TOOLS_SHADER})
add_executable(vela-slowgpu vela-slowgpu.c ${TOOLS_SHADER} ${CONTENTION_SHADER})
add_test(NAME benchmark-slowgpu-cli COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/benchmarks/tests/slowgpu_cli.py $<TARGET_FILE:vela-slowgpu>)
set_tests_properties(benchmark-slowgpu-cli PROPERTIES TIMEOUT 20)
if(TARGET vela-bench-desktop AND BENCH_KWIN AND BENCH_DBUS_RUN)
    add_test(NAME benchmark-plasma-contention-integration COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/benchmarks/tests/plasma_scaling_integration.py $<TARGET_FILE:vela-bench-desktop> --contention)
    set_tests_properties(benchmark-plasma-contention-integration PROPERTIES TIMEOUT 150 LABELS "gpu" RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
endif()
if(TARGET vela-bench-desktop AND BENCH_DBUS_RUN AND VELA_BUILD_COMPOSITOR)
    add_test(NAME benchmark-p3-loads-integration COMMAND ${BENCH_DBUS_RUN} -- ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/benchmarks/tests/desktop_integration.py slow-apps)
    set_tests_properties(benchmark-p3-loads-integration PROPERTIES TIMEOUT 900 LABELS "gpu" RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77 ENVIRONMENT "VELA_BUILD=${CMAKE_BINARY_DIR}")
endif()
if(VELA_BUILD_COMPOSITOR)
    add_test(NAME benchmark-p2-contention-integration COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/benchmarks/tests/contention_integration.py)
    set_tests_properties(benchmark-p2-contention-integration PROPERTIES TIMEOUT 280 LABELS "gpu"
        RUN_SERIAL TRUE SKIP_RETURN_CODE 77 ENVIRONMENT "VELA_BUILD=${CMAKE_BINARY_DIR}")
endif()
target_include_directories(vela-slowgpu PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/shaders)
target_link_libraries(vela-slowgpu PRIVATE proto-xdg-shell proto-linux-dmabuf proto-linux-drm-syncobj proto-presentation-time
    PkgConfig::TOOLS_VULKAN PkgConfig::TOOLS_GBM PkgConfig::TOOLS_DRM)
endif()

foreach(tool vela-windows vela-input vela-pattern vela-testlock vela-panel vela-randr)
    target_compile_options(${tool} PRIVATE -Wall -Wextra -Wno-unused-parameter)
endforeach()
if(VELA_BUILD_GPU_TEST_TOOLS)
    target_compile_options(vela-slowgpu PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

endif() # VELA_BUILD_TOOLS
