# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(PkgConfig REQUIRED)

# wlroots changes API at every minor version and installs a pkg-config file
# with the version in its name. Vela is written for 0.20 (sway 1.12's); with
# -DVELA_WLROOTS=wlroots-0.19 it should also build against 0.19.
set(VELA_WLROOTS "wlroots-0.20" CACHE STRING "pkg-config name of wlroots")

pkg_check_modules(WLROOTS REQUIRED IMPORTED_TARGET ${VELA_WLROOTS})
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(PIXMAN REQUIRED IMPORTED_TARGET pixman-1)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
# Vela's renderer (docs/renderer.md): Vulkan, output buffers with GBM.
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan>=1.4)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
# The compositor's text (window titles): KDE's fonts through fontconfig.
pkg_check_modules(FONTS REQUIRED IMPORTED_TARGET freetype2 harfbuzz fontconfig)
# App icons in the title bar (SVG): optional, without it no icons.
pkg_check_modules(RSVG IMPORTED_TARGET librsvg-2.0 cairo)
# The X11 window manager connection (xwayland.c), when wlroots has Xwayland.
pkg_check_modules(XCB IMPORTED_TARGET xcb)
find_program(GLSLC NAMES glslc REQUIRED)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.32)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

# Some wlroots headers include the protocols' "server" headers. With wlroots
# 0.19 every compositor must generate them itself; 0.20 installs some, but
# generating them here (as sway does) works with both.
set(VELA_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${VELA_GENERATED_DIR})
set(VELA_PROTOCOL_HEADERS)

function(vela_server_protocol xml name)
    set(header ${VELA_GENERATED_DIR}/${name}-protocol.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${WAYLAND_SCANNER} server-header ${xml} ${header}
        DEPENDS ${xml}
        COMMENT "Generating ${name}-protocol.h")
    list(APPEND VELA_PROTOCOL_HEADERS ${header})
    set(VELA_PROTOCOL_HEADERS ${VELA_PROTOCOL_HEADERS} PARENT_SCOPE)
endfunction()

vela_server_protocol(${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml xdg-shell)
vela_server_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/cursor-shape/cursor-shape-v1.xml cursor-shape-v1)
vela_server_protocol(${PROJECT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml
    wlr-layer-shell-unstable-v1)

# Protocols wlroots doesn't implement: we implement them, so the interface code
# is needed too.
set(VELA_OWN_PROTOCOL_SOURCES)
function(vela_own_protocol xml name)
    set(header ${VELA_GENERATED_DIR}/${name}-protocol.h)
    set(code ${VELA_GENERATED_DIR}/${name}-protocol.c)
    add_custom_command(
        OUTPUT ${header} ${code}
        COMMAND ${WAYLAND_SCANNER} server-header ${xml} ${header}
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${code}
        DEPENDS ${xml}
        COMMENT "Generating the ${name} server")
    list(APPEND VELA_OWN_PROTOCOL_SOURCES ${header} ${code})
    set(VELA_OWN_PROTOCOL_SOURCES ${VELA_OWN_PROTOCOL_SOURCES} PARENT_SCOPE)
endfunction()

vela_own_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/ext-background-effect/ext-background-effect-v1.xml
    ext-background-effect-v1)
vela_own_protocol(${PROJECT_SOURCE_DIR}/protocols/vela-output-preview-v1.xml vela-output-preview-v1)

# Client side, to talk to the host compositor when Vela runs in a window
# (nested.c).
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
set(VELA_CLIENT_PROTOCOL_SOURCES)
function(vela_client_protocol xml name)
    set(header ${VELA_GENERATED_DIR}/${name}-client-protocol.h)
    set(code ${VELA_GENERATED_DIR}/${name}-protocol.c)
    add_custom_command(
        OUTPUT ${header} ${code}
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${header}
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${code}
        DEPENDS ${xml}
        COMMENT "Generating the ${name} client")
    list(APPEND VELA_CLIENT_PROTOCOL_SOURCES ${header} ${code})
    set(VELA_CLIENT_PROTOCOL_SOURCES ${VELA_CLIENT_PROTOCOL_SOURCES} PARENT_SCOPE)
endfunction()

vela_client_protocol(${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml viewporter)
vela_client_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/fractional-scale/fractional-scale-v1.xml fractional-scale-v1)
vela_client_protocol(${WAYLAND_PROTOCOLS_DIR}/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml
    keyboard-shortcuts-inhibit-unstable-v1)

# Shaders: GLSL -> SPIR-V during the build, included in the executable as a
# list of numbers ("name.spv.inc", to put inside a uint32_t array).
set(VELA_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${VELA_SHADER_DIR})
set(VELA_SHADERS)
foreach(shader quad.vert texture.frag rect.frag shadow.frag blur_down.frag blur_up.frag blur_mix.frag)
    set(source ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${shader})
    set(output ${VELA_SHADER_DIR}/${shader}.spv.inc)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${GLSLC} --target-env=vulkan1.4 -O -mfmt=num -o ${output} ${source}
        DEPENDS ${source} ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/push.glsl
            ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/shape.glsl
        COMMENT "Compilo lo shader ${shader}")
    list(APPEND VELA_SHADERS ${output})
endforeach()

# The modules without wlroots or a GPU (pure logic, covered by the tests):
# shared by the compositor and the tests.
add_library(vela-pure STATIC
    src/backlight_choice.c
    src/brightness_model.c
    src/buffer.c
    src/color.c
    src/config.c
    src/diagnostics.c
    src/frame_clock.c
    src/geometry.c
    src/legacy_names.c
    src/motion.c
    src/output_config.c
    src/output_key.c
    src/path_security.c
    src/sun.c
    src/sync.c
    src/util.c)
target_include_directories(vela-pure PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(vela-pure PUBLIC _GNU_SOURCE)
target_compile_options(vela-pure PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(vela-pure PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(vela-pure PUBLIC ${MATH_LIBRARY})
endif()

add_executable(vela-compositor
    src/a11y.c
    src/error_screen.c
    src/bindings.c
    src/bluetooth.c
    src/brightness.c
    src/command.c
    src/decoration.c
    src/focus.c
    src/icons.c
    src/input.c
    src/interact.c
    src/keyboard.c
    src/layer.c
    src/lock.c
    src/main.c
    src/nested.c
    src/output.c
    src/output_manager.c
    src/output_preview.c
    src/popup.c
    src/polkit.c
    src/process.c
    src/server.c
    src/session.c
    src/shell.c
    src/snap.c
    src/snapshot.c
    src/supervisor.c
    src/supervisor_children.c
    src/switcher.c
    src/text.c
    src/view.c
    src/workspace.c
    src/xwayland.c
    src/render/allocator.c
    src/render/formats.c
    src/render/pass.c
    src/render/pixel_buffer.c
    src/render/renderer.c
    src/render/wait_arrays.c
    src/render/texture.c
    src/render/vulkan.c
    src/scene/capture.c
    src/scene/effects.c
    src/scene/frame.c
    src/scene/ready.c
    src/scene/scene.c
    src/scene/surface.c
    ${VELA_SHADERS}
    ${VELA_PROTOCOL_HEADERS}
    ${VELA_CLIENT_PROTOCOL_SOURCES}
    ${VELA_OWN_PROTOCOL_SOURCES})

target_include_directories(vela-compositor PRIVATE
    ${VELA_GENERATED_DIR} ${VELA_SHADER_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(vela-compositor PRIVATE WLR_USE_UNSTABLE
    VELA_LIBEXECDIR="${CMAKE_INSTALL_FULL_LIBEXECDIR}")
# With Vulkan, structs are initialized naming only the fields needed (the
# others are zero): the warning about omitted fields would be noise.
target_compile_options(vela-compositor PRIVATE -Wall -Wextra -Wno-unused-parameter
    -Wno-missing-field-initializers)
set_target_properties(vela-compositor PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
# logind lock hints are best-effort when libsystemd is available.
pkg_check_modules(VELA_LOCK_SYSTEMD IMPORTED_TARGET libsystemd)
if(VELA_LOCK_SYSTEMD_FOUND)
    target_compile_definitions(vela-compositor PRIVATE VELA_HAVE_SYSTEMD_LOCK_HINT=1)
    target_link_libraries(vela-compositor PRIVATE PkgConfig::VELA_LOCK_SYSTEMD)
endif()

target_link_libraries(vela-compositor PRIVATE
    vela-pure
    PkgConfig::WLROOTS
    PkgConfig::WAYLAND_SERVER
    PkgConfig::WAYLAND_CLIENT
    PkgConfig::XKBCOMMON
    PkgConfig::PIXMAN
    PkgConfig::LIBINPUT
    PkgConfig::VULKAN
    PkgConfig::GBM
    PkgConfig::LIBDRM
    PkgConfig::FONTS)
if(XCB_FOUND)
    target_link_libraries(vela-compositor PRIVATE PkgConfig::XCB)
endif()
if(RSVG_FOUND)
    target_link_libraries(vela-compositor PRIVATE PkgConfig::RSVG)
    target_compile_definitions(vela-compositor PRIVATE VELA_HAVE_RSVG=1)
else()
    message(STATUS "librsvg not found: the title bar will have no icons")
endif()

install(TARGETS vela-compositor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The compositor tests (ctest), without a GPU or wlroots: frame timing, curves,
# sharpness geometry, color, sun, settings. They need GoogleTest (the gtest
# package) and the test tools enabled.
if(VELA_BUILD_TOOLS)
    # GPU-free policy test: verifies that the cursor-specific flags cannot
    # leak into ordinary output/capture allocations.
    add_executable(vela-cursor-policy-test tests/cursor_policy_test.c)
    target_include_directories(vela-cursor-policy-test PRIVATE src)
    target_compile_definitions(vela-cursor-policy-test PRIVATE WLR_USE_UNSTABLE)
    target_link_libraries(vela-cursor-policy-test PRIVATE PkgConfig::GBM PkgConfig::LIBDRM PkgConfig::WLROOTS)
    target_compile_options(vela-cursor-policy-test PRIVATE -Wall -Wextra -Werror)
    set_target_properties(vela-cursor-policy-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    add_test(NAME compositor.cursor-policy COMMAND vela-cursor-policy-test)

    # Exercise the actual display protocol and watchdog without Vulkan/DRM.
    add_executable(vela-display-test-server
        ${PROJECT_SOURCE_DIR}/settings/tests/display_server.c
        src/output_manager.c src/output_preview.c ${VELA_OWN_PROTOCOL_SOURCES})
    target_include_directories(vela-display-test-server PRIVATE src ${VELA_GENERATED_DIR})
    target_compile_definitions(vela-display-test-server PRIVATE WLR_USE_UNSTABLE
        VELA_OUTPUT_PREVIEW_TIMEOUT_MS=1200)
    target_compile_options(vela-display-test-server PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(vela-display-test-server PRIVATE vela-pure PkgConfig::WLROOTS
        PkgConfig::WAYLAND_SERVER)

    # CPU scene comparison, using the actual private frame function. Discard
    # unrelated rendering sections so these tools need no compositor or GPU.
    set(VELA_SCENE_DIFF_REFERENCE "" CACHE FILEPATH
        "Optional compatible frame.c from a prior revision, for scene-diff benchmarking")
    set(diff_drivers vela-scene-diff-driver)
    set(diff_targets vela-scene-diff-bench vela-scene-diff-test)
    if(VELA_SCENE_DIFF_REFERENCE)
        list(APPEND diff_drivers vela-scene-diff-reference-driver)
        list(APPEND diff_targets vela-scene-diff-reference)
    endif()
    foreach(driver IN LISTS diff_drivers)
        add_library(${driver} OBJECT tests/frame_diff_driver.c ${VELA_PROTOCOL_HEADERS})
        target_include_directories(${driver} PRIVATE src ${VELA_GENERATED_DIR})
        target_compile_definitions(${driver} PRIVATE WLR_USE_UNSTABLE)
        target_compile_options(${driver} PRIVATE -ffunction-sections -fdata-sections
            -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
        target_link_libraries(${driver} PRIVATE vela-pure PkgConfig::WLROOTS
            PkgConfig::WAYLAND_SERVER PkgConfig::PIXMAN)
        set_target_properties(${driver} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    endforeach()
    if(VELA_SCENE_DIFF_REFERENCE)
        get_filename_component(diff_reference "${VELA_SCENE_DIFF_REFERENCE}" ABSOLUTE)
        target_compile_definitions(vela-scene-diff-reference-driver PRIVATE VELA_FRAME_SOURCE="${diff_reference}")
    endif()
    foreach(target IN LISTS diff_targets)
        set(driver vela-scene-diff-driver)
        if(target STREQUAL "vela-scene-diff-reference")
            set(driver vela-scene-diff-reference-driver)
        endif()
        add_executable(${target} tests/frame_diff_bench.c $<TARGET_OBJECTS:${driver}>)
        target_include_directories(${target} PRIVATE src)
        target_compile_definitions(${target} PRIVATE WLR_USE_UNSTABLE)
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        target_link_options(${target} PRIVATE -Wl,--gc-sections)
        target_link_libraries(${target} PRIVATE vela-pure PkgConfig::WLROOTS
            PkgConfig::WAYLAND_SERVER PkgConfig::PIXMAN)
        set_target_properties(${target} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    endforeach()
    target_compile_definitions(vela-scene-diff-test PRIVATE VELA_DIFF_ALLOCATION_TEST)
    target_link_options(vela-scene-diff-test PRIVATE -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free)
    add_test(NAME compositor.scene-diff COMMAND vela-scene-diff-test --test)

    # The locker recovery state machine must be exercised on CPU-only CI.
    add_executable(vela-lock-retry-test tests/lock_retry_policy_test.c)
    target_include_directories(vela-lock-retry-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(vela-lock-retry-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    target_compile_options(vela-lock-retry-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME compositor.lock-retry-policy COMMAND vela-lock-retry-test)

    # CPU-only Vulkan wait-array tests: force failure of either realloc and
    # verify independent capacities and steady-state allocation reuse.
    add_executable(vela-wait-arrays-oom-test
        tests/wait_arrays_oom_test.c src/render/wait_arrays.c)
    target_include_directories(vela-wait-arrays-oom-test PRIVATE src)
    target_compile_options(vela-wait-arrays-oom-test PRIVATE -Wall -Wextra -Werror)
    target_link_options(vela-wait-arrays-oom-test PRIVATE -Wl,--wrap=realloc)
    target_link_libraries(vela-wait-arrays-oom-test PRIVATE vela-pure PkgConfig::VULKAN)
    set_target_properties(vela-wait-arrays-oom-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    add_test(NAME compositor.wait-arrays-oom COMMAND vela-wait-arrays-oom-test)

    # Linux /proc enumeration and SIGTERM/SIGKILL child cleanup, no GPU.
    add_executable(vela-supervisor-children-test
        tests/supervisor_children_test.c src/supervisor_children.c)
    target_include_directories(vela-supervisor-children-test PRIVATE src)
    target_compile_definitions(vela-supervisor-children-test PRIVATE _GNU_SOURCE)
    target_compile_options(vela-supervisor-children-test PRIVATE -Wall -Wextra -Werror)
    set_target_properties(vela-supervisor-children-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    add_test(NAME compositor.supervisor-children COMMAND vela-supervisor-children-test)

    find_package(GTest)
    if(GTest_FOUND)
        add_executable(vela-compositor-tests
            tests/backlight_choice_test.cpp
            tests/brightness_model_test.cpp
            tests/frame_clock_test.cpp
            tests/motion_test.cpp
            tests/geometry_test.cpp
            tests/color_sun_test.cpp
            tests/settings_test.cpp
            tests/output_config_test.cpp
            tests/output_key_test.cpp
            tests/path_security_test.cpp
            tests/diagnostics_test.cpp
            tests/sync_test.cpp
            tests/buffer_test.cpp)
        target_compile_options(vela-compositor-tests PRIVATE -Wall -Wextra)
        target_link_libraries(vela-compositor-tests PRIVATE vela-pure GTest::gtest_main)
        include(GoogleTest)
        gtest_discover_tests(vela-compositor-tests TEST_PREFIX "compositor.")
    else()
        message(STATUS "GoogleTest not found: no compositor tests (package gtest)")
    endif()
endif()
