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

# Protocols implemented by Vela need interface code, including the output
# capture adapter and its referenced foreign-toplevel interface.
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
vela_own_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-capture-source/ext-image-capture-source-v1.xml
    ext-image-capture-source-v1)
vela_own_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
    ext-foreign-toplevel-list-v1)
# Xwayland's own xdg-output: the outputs in pixels (x11_root.c).
vela_own_protocol(${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml
    xdg-output-unstable-v1)

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
    src/desktop_conf.c
    src/diagnostics.c
    src/frame_clock.c
    src/geometry.c
    src/icon_cache.c
    src/legacy_names.c
    src/lock_state.c
    src/motion.c
    src/output_config.c
    src/output_key.c
    src/path_security.c
    src/sun.c
    src/supervisor_child.c
    src/sync.c
    src/util.c
    src/x11_space.c)
target_include_directories(vela-pure PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(vela-pure PUBLIC _GNU_SOURCE)
target_compile_options(vela-pure PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(vela-pure PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(vela-pure PUBLIC vela-alloc)
find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(vela-pure PUBLIC ${MATH_LIBRARY})
endif()

# The compositor's objects, compiled once: the installed executable links
# them, and so does the test build that counts allocations (below).
add_library(vela-compositor-objects OBJECT
    src/a11y.c
    src/capture_watch.c
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
    src/updates.c
    src/view.c
    src/workspace.c
    src/x11_root.c
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
    src/scene/output_capture.c
    src/scene/effects.c
    src/scene/frame.c
    src/scene/ready.c
    src/scene/scene.c
    src/scene/surface.c
    ${VELA_SHADERS}
    ${VELA_PROTOCOL_HEADERS}
    ${VELA_CLIENT_PROTOCOL_SOURCES}
    ${VELA_OWN_PROTOCOL_SOURCES})

target_include_directories(vela-compositor-objects PRIVATE
    ${VELA_GENERATED_DIR} ${VELA_SHADER_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(vela-compositor-objects PRIVATE WLR_USE_UNSTABLE
    VELA_LIBEXECDIR="${CMAKE_INSTALL_FULL_LIBEXECDIR}")
# With Vulkan, structs are initialized naming only the fields needed (the
# others are zero): the warning about omitted fields would be noise.
target_compile_options(vela-compositor-objects PRIVATE -Wall -Wextra -Wno-unused-parameter
    -Wno-missing-field-initializers)
set_target_properties(vela-compositor-objects PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
# logind lock hints are best-effort when libsystemd is available.
pkg_check_modules(VELA_LOCK_SYSTEMD IMPORTED_TARGET libsystemd)
if(VELA_LOCK_SYSTEMD_FOUND)
    target_compile_definitions(vela-compositor-objects PRIVATE VELA_HAVE_SYSTEMD_LOCK_HINT=1)
    target_link_libraries(vela-compositor-objects PUBLIC PkgConfig::VELA_LOCK_SYSTEMD)
endif()

target_link_libraries(vela-compositor-objects PUBLIC
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
    target_link_libraries(vela-compositor-objects PUBLIC PkgConfig::XCB)
endif()
if(RSVG_FOUND)
    target_link_libraries(vela-compositor-objects PUBLIC PkgConfig::RSVG)
    target_compile_definitions(vela-compositor-objects PRIVATE VELA_HAVE_RSVG=1)
else()
    message(STATUS "librsvg not found: the title bar will have no icons")
endif()

add_executable(vela-compositor)
target_link_libraries(vela-compositor PRIVATE vela-compositor-objects)

install(TARGETS vela-compositor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The compositor tests (ctest), without a GPU or wlroots: frame timing, curves,
# sharpness geometry, color, sun, settings. They need GoogleTest (the gtest
# package) and the test tools enabled.
if(VELA_BUILD_TOOLS)
    # The LD_PRELOAD test shims below are never instrumented. With ASan its
    # runtime must come first in the initial library list, so a test's
    # LD_PRELOAD becomes "<asan runtime>:<shim>"; ASan's interceptors then
    # reach the shim as the next definition.
    set(VELA_TEST_PRELOAD_PREFIX "")
    set(VELA_TEST_ASAN OFF)
    if(CMAKE_C_FLAGS MATCHES "-fsanitize=[^ ]*address")
        set(VELA_TEST_ASAN ON)
        foreach(runtime libasan.so libclang_rt.asan-x86_64.so)
            execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=${runtime}
                OUTPUT_VARIABLE asan_runtime OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(IS_ABSOLUTE "${asan_runtime}" AND EXISTS "${asan_runtime}")
                set(VELA_TEST_PRELOAD_PREFIX "${asan_runtime}:")
                break()
            endif()
        endforeach()
    endif()
    set(VELA_TEST_PRELOAD_PREFIX "${VELA_TEST_PRELOAD_PREFIX}" CACHE INTERNAL "ASan runtime before test shims")

    # The helpers' entry points, wrapped by tests/alloc_count.c to count the
    # calls of Vela's own code (docs/memory-allocation-policy.md, "Frame path").
    set(VELA_ALLOC_COUNT_WRAP -Wl,--wrap=vela_alloc_at -Wl,--wrap=vela_grow_size_at
        -Wl,--wrap=vela_grow_int_at -Wl,--wrap=vela_strdup_at -Wl,--wrap=vela_strndup_at
        -Wl,--wrap=vela_asprintf_at)
    # The same compositor, objects and all, with the counter: the functional
    # frame-path test (tests/functional/test_frame_path.py) runs it. Never
    # installed.
    add_executable(vela-compositor-alloc-count tests/alloc_count.c)
    target_link_libraries(vela-compositor-alloc-count PRIVATE vela-compositor-objects)
    target_link_options(vela-compositor-alloc-count PRIVATE ${VELA_ALLOC_COUNT_WRAP})
    target_compile_options(vela-compositor-alloc-count PRIVATE -Wall -Wextra -Werror)
    set_target_properties(vela-compositor-alloc-count PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)

    # LD_PRELOAD shim that makes one pixman region operation really fail
    # (tests/functional/test_session.py, damage fallback).
    add_library(vela-pixman-fail-preload MODULE tests/pixman_fail_preload.c)
    target_link_libraries(vela-pixman-fail-preload PRIVATE PkgConfig::PIXMAN ${CMAKE_DL_LIBS})
    target_compile_options(vela-pixman-fail-preload PRIVATE -Wall -Wextra -Werror -fno-sanitize=all)
    set_target_properties(vela-pixman-fail-preload PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)

    # LD_PRELOAD shim that makes getline fail partway through a chosen file,
    # and the configuration-read test that runs under it.
    add_library(vela-getline-fail-preload MODULE tests/getline_fail_preload.c)
    target_link_libraries(vela-getline-fail-preload PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(vela-getline-fail-preload PRIVATE -Wall -Wextra -Werror -fno-sanitize=all)
    set_target_properties(vela-getline-fail-preload PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    # sd-event sources that fail as with descriptors exhausted, for the
    # daemons' tests (tests/updates, tests/bluetooth).
    add_library(vela-sd-event-fail-preload MODULE tests/sd_event_fail_preload.c)
    target_link_libraries(vela-sd-event-fail-preload PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(vela-sd-event-fail-preload PRIVATE -Wall -Wextra -Werror -fno-sanitize=all)
    set_target_properties(vela-sd-event-fail-preload PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    # setenv and fstatat failures on the supervisor's restart path.
    add_library(vela-libc-fail-preload MODULE tests/libc_fail_preload.c)
    target_link_libraries(vela-libc-fail-preload PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(vela-libc-fail-preload PRIVATE -Wall -Wextra -Werror -fno-sanitize=all)
    set_target_properties(vela-libc-fail-preload PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    add_executable(vela-config-read-test tests/config_read_test.c)
    target_link_libraries(vela-config-read-test PRIVATE vela-pure)
    target_compile_options(vela-config-read-test PRIVATE -Wall -Wextra -Werror)
    set_target_properties(vela-config-read-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    add_dependencies(vela-config-read-test vela-getline-fail-preload)
    # The ENOMEM case aborts in a child: flush its coverage.
    if(CMAKE_C_FLAGS MATCHES "--coverage")
        target_compile_definitions(vela-config-read-test PRIVATE VELA_TEST_COVERAGE)
    endif()
    add_test(NAME compositor.config-read COMMAND vela-config-read-test
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(compositor.config-read PROPERTIES
        ENVIRONMENT "LD_PRELOAD=${VELA_TEST_PRELOAD_PREFIX}$<TARGET_FILE:vela-getline-fail-preload>")

    add_executable(vela-device-loss-policy-test tests/device_loss_policy_test.c)
    target_include_directories(vela-device-loss-policy-test PRIVATE src)
    set_target_properties(vela-device-loss-policy-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    target_compile_options(vela-device-loss-policy-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME compositor.device-loss-policy COMMAND vela-device-loss-policy-test)

    # Run the production vkCreateDevice/vela_vulkan_create error path against
    # deterministic entry-point stubs, without opening a GPU or Wayland session.
    add_executable(vela-vulkan-device-init-test tests/vulkan_device_init_test.c src/render/formats.c)
    target_include_directories(vela-vulkan-device-init-test PRIVATE src)
    target_compile_definitions(vela-vulkan-device-init-test PRIVATE WLR_USE_UNSTABLE)
    target_compile_options(vela-vulkan-device-init-test PRIVATE -ffunction-sections -fdata-sections
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    target_link_options(vela-vulkan-device-init-test PRIVATE -Wl,--gc-sections)
    target_link_libraries(vela-vulkan-device-init-test PRIVATE vela-pure PkgConfig::VULKAN
        PkgConfig::WLROOTS PkgConfig::WAYLAND_SERVER PkgConfig::LIBDRM)
    set_target_properties(vela-vulkan-device-init-test PROPERTIES C_STANDARD 17
        C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME compositor.vulkan-device-init COMMAND vela-vulkan-device-init-test)

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

    # The frame path's damage fallback on the CPU, with real pixman failures
    # (the test's own malloc). Not with ASan, whose allocator it would replace.
    if(NOT VELA_TEST_ASAN)
        add_executable(vela-damage-fallback-test tests/damage_fallback_test.c ${VELA_PROTOCOL_HEADERS})
        target_include_directories(vela-damage-fallback-test PRIVATE src ${VELA_GENERATED_DIR})
        target_compile_definitions(vela-damage-fallback-test PRIVATE WLR_USE_UNSTABLE)
        target_compile_options(vela-damage-fallback-test PRIVATE -ffunction-sections -fdata-sections
            -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function)
        target_link_options(vela-damage-fallback-test PRIVATE -Wl,--gc-sections)
        target_link_libraries(vela-damage-fallback-test PRIVATE vela-pure PkgConfig::WLROOTS
            PkgConfig::WAYLAND_SERVER PkgConfig::PIXMAN)
        set_target_properties(vela-damage-fallback-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)
        add_test(NAME compositor.damage-fallback COMMAND vela-damage-fallback-test)
    endif()
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

    # Production readback helpers: packed alpha, untouched padding and Vulkan
    # allocation/map failures, with Vulkan entry points stubbed instead of a GPU.
    add_executable(vela-readback-test tests/readback_test.c src/render/formats.c
        src/render/wait_arrays.c ${VELA_SHADERS})
    target_include_directories(vela-readback-test PRIVATE src ${VELA_SHADER_DIR} ${VELA_GENERATED_DIR})
    target_compile_definitions(vela-readback-test PRIVATE WLR_USE_UNSTABLE _POSIX_C_SOURCE=200809L)
    target_compile_options(vela-readback-test PRIVATE -ffunction-sections -fdata-sections
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    target_link_options(vela-readback-test PRIVATE -Wl,--gc-sections)
    target_link_libraries(vela-readback-test PRIVATE vela-pure PkgConfig::WLROOTS
        PkgConfig::VULKAN PkgConfig::WAYLAND_SERVER PkgConfig::LIBDRM PkgConfig::PIXMAN)
    set_target_properties(vela-readback-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME compositor.readback COMMAND vela-readback-test)

    # The frame path's drawing without a GPU: the production pass, renderer and
    # texture code with Vulkan stubbed, counting the helpers' calls
    # (docs/memory-allocation-policy.md, "Frame path"). libvulkan is not
    # linked, so an entry point without its stub fails at link time instead
    # of reaching the real loader with fake handles.
    add_executable(vela-frame-path-test tests/frame_path_test.c tests/alloc_count.c src/render/formats.c
        src/render/wait_arrays.c ${VELA_SHADERS})
    target_include_directories(vela-frame-path-test PRIVATE src tests ${VELA_SHADER_DIR} ${VELA_GENERATED_DIR}
        ${VULKAN_INCLUDE_DIRS})
    target_compile_definitions(vela-frame-path-test PRIVATE WLR_USE_UNSTABLE _GNU_SOURCE)
    target_compile_options(vela-frame-path-test PRIVATE -ffunction-sections -fdata-sections
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    target_link_options(vela-frame-path-test PRIVATE -Wl,--gc-sections ${VELA_ALLOC_COUNT_WRAP})
    target_link_libraries(vela-frame-path-test PRIVATE vela-pure PkgConfig::WLROOTS
        PkgConfig::WAYLAND_SERVER PkgConfig::LIBDRM PkgConfig::PIXMAN)
    set_target_properties(vela-frame-path-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME compositor.frame-path COMMAND vela-frame-path-test)

    # Real output-source damage/idle policy with GPU work replaced by terminal
    # frame notifications; no private wlroots session state is accessed.
    add_executable(vela-capture-damage-test tests/capture_damage_test.c ${VELA_OWN_PROTOCOL_SOURCES})
    target_include_directories(vela-capture-damage-test PRIVATE src ${VELA_GENERATED_DIR})
    target_compile_definitions(vela-capture-damage-test PRIVATE WLR_USE_UNSTABLE _POSIX_C_SOURCE=200809L)
    target_compile_options(vela-capture-damage-test PRIVATE -ffunction-sections -fdata-sections
        -Wall -Wextra -Wno-unused-parameter)
    target_link_options(vela-capture-damage-test PRIVATE -Wl,--gc-sections)
    target_link_libraries(vela-capture-damage-test PRIVATE vela-alloc PkgConfig::WLROOTS
        PkgConfig::WAYLAND_SERVER PkgConfig::LIBDRM PkgConfig::PIXMAN PkgConfig::VULKAN m)
    set_target_properties(vela-capture-damage-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME compositor.capture-damage COMMAND vela-capture-damage-test)

    # Linux /proc enumeration and SIGTERM/SIGKILL child cleanup, no GPU.
    add_executable(vela-supervisor-children-test
        tests/supervisor_children_test.c src/supervisor_children.c)
    target_include_directories(vela-supervisor-children-test PRIVATE src)
    target_compile_definitions(vela-supervisor-children-test PRIVATE _GNU_SOURCE)
    target_compile_options(vela-supervisor-children-test PRIVATE -Wall -Wextra -Werror)
    set_target_properties(vela-supervisor-children-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    add_test(NAME compositor.supervisor-children COMMAND vela-supervisor-children-test)

    # Xwayland's own xdg-output: the global filter, the outputs in pixels and
    # their updates, with headless wlroots outputs and two clients, no GPU.
    set(xdg_output_client ${VELA_GENERATED_DIR}/xdg-output-unstable-v1-client-protocol.h)
    add_custom_command(
        OUTPUT ${xdg_output_client}
        COMMAND ${WAYLAND_SCANNER} client-header
            ${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml ${xdg_output_client}
        DEPENDS ${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/xdg-output-unstable-v1.xml
        COMMENT "Generating xdg-output-unstable-v1-client-protocol.h")
    add_executable(vela-x11-root-test tests/x11_root_test.c src/x11_root.c ${VELA_OWN_PROTOCOL_SOURCES}
        ${VELA_PROTOCOL_HEADERS} ${xdg_output_client})
    target_include_directories(vela-x11-root-test PRIVATE src ${VELA_GENERATED_DIR})
    target_compile_definitions(vela-x11-root-test PRIVATE WLR_USE_UNSTABLE _POSIX_C_SOURCE=200809L)
    target_compile_options(vela-x11-root-test PRIVATE -Wall -Wextra -Wno-unused-parameter
        -Wno-missing-field-initializers)
    target_link_libraries(vela-x11-root-test PRIVATE vela-pure PkgConfig::WLROOTS PkgConfig::WAYLAND_SERVER
        PkgConfig::WAYLAND_CLIENT PkgConfig::PIXMAN)
    set_target_properties(vela-x11-root-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME compositor.x11-root COMMAND vela-x11-root-test)

    # Who captures the screen (capture_watch.c, docs/screen-sharing.md): wlroots'
    # capture managers, a headless output drawn by pixman, clients in this
    # process and a clock the test moves; no GPU.
    set(capture_watch_clients)
    foreach(protocol
            staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1
            staging/ext-image-capture-source/ext-image-capture-source-v1
            staging/ext-image-copy-capture/ext-image-copy-capture-v1)
        get_filename_component(name ${protocol} NAME)
        set(header ${VELA_GENERATED_DIR}/${name}-client-protocol.h)
        add_custom_command(OUTPUT ${header}
            COMMAND ${WAYLAND_SCANNER} client-header ${WAYLAND_PROTOCOLS_DIR}/${protocol}.xml ${header}
            DEPENDS ${WAYLAND_PROTOCOLS_DIR}/${protocol}.xml
            COMMENT "Generating ${name}-client-protocol.h")
        list(APPEND capture_watch_clients ${header})
    endforeach()
    # Not among Vela's own protocols: their interface code too.
    foreach(protocol
            ${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
            ${PROJECT_SOURCE_DIR}/protocols/wlr-screencopy-unstable-v1.xml)
        get_filename_component(name ${protocol} NAME_WE)
        set(code ${VELA_GENERATED_DIR}/${name}-client-protocol.c)
        add_custom_command(OUTPUT ${code}
            COMMAND ${WAYLAND_SCANNER} private-code ${protocol} ${code}
            DEPENDS ${protocol}
            COMMENT "Generating ${name}-client-protocol.c")
        list(APPEND capture_watch_clients ${code})
    endforeach()
    set(screencopy_client ${VELA_GENERATED_DIR}/wlr-screencopy-unstable-v1-client-protocol.h)
    add_custom_command(OUTPUT ${screencopy_client}
        COMMAND ${WAYLAND_SCANNER} client-header ${PROJECT_SOURCE_DIR}/protocols/wlr-screencopy-unstable-v1.xml
            ${screencopy_client}
        DEPENDS ${PROJECT_SOURCE_DIR}/protocols/wlr-screencopy-unstable-v1.xml
        COMMENT "Generating wlr-screencopy-unstable-v1-client-protocol.h")
    add_executable(vela-capture-watch-test tests/capture_watch_test.c src/capture_watch.c src/buffer.c
        ${VELA_OWN_PROTOCOL_SOURCES} ${VELA_PROTOCOL_HEADERS} ${capture_watch_clients} ${screencopy_client})
    target_include_directories(vela-capture-watch-test PRIVATE src ${VELA_GENERATED_DIR})
    target_compile_definitions(vela-capture-watch-test PRIVATE WLR_USE_UNSTABLE _GNU_SOURCE)
    target_compile_options(vela-capture-watch-test PRIVATE -Wall -Wextra -Wno-unused-parameter
        -Wno-missing-field-initializers)
    target_link_libraries(vela-capture-watch-test PRIVATE vela-alloc PkgConfig::WLROOTS PkgConfig::WAYLAND_SERVER
        PkgConfig::WAYLAND_CLIENT PkgConfig::PIXMAN)
    set_target_properties(vela-capture-watch-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    add_test(NAME compositor.capture-watch COMMAND vela-capture-watch-test)

    # The compositor's children on a real event loop, no GPU: an exit reaches
    # its callback, and a child that can't be watched is reaped.
    add_executable(vela-process-test tests/process_test.c src/process.c)
    target_include_directories(vela-process-test PRIVATE src)
    target_compile_definitions(vela-process-test PRIVATE VELA_LIBEXECDIR="${CMAKE_INSTALL_FULL_LIBEXECDIR}")
    target_compile_options(vela-process-test PRIVATE -Wall -Wextra -Wno-unused-parameter -Werror)
    target_link_libraries(vela-process-test PRIVATE vela-pure PkgConfig::WLROOTS PkgConfig::WAYLAND_SERVER)
    set_target_properties(vela-process-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    add_test(NAME compositor.process COMMAND vela-process-test)
    set_tests_properties(compositor.process PROPERTIES TIMEOUT 30)

    # Which vela-update-daemon the compositor starts, if any.
    add_executable(vela-updates-start-test tests/updates_start_test.c src/updates.c)
    target_include_directories(vela-updates-start-test PRIVATE src ${VELA_GENERATED_DIR})
    target_compile_definitions(vela-updates-start-test PRIVATE WLR_USE_UNSTABLE _POSIX_C_SOURCE=200809L)
    target_compile_options(vela-updates-start-test PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(vela-updates-start-test PRIVATE PkgConfig::WLROOTS PkgConfig::WAYLAND_SERVER
        PkgConfig::PIXMAN)
    set_target_properties(vela-updates-start-test PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME compositor.updates-start COMMAND vela-updates-start-test)

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
            tests/lock_state_test.cpp
            tests/supervisor_child_test.cpp
            tests/diagnostics_test.cpp
            tests/sync_test.cpp
            tests/buffer_test.cpp
            tests/x11_space_test.cpp)
        target_compile_options(vela-compositor-tests PRIVATE -Wall -Wextra)
        target_link_libraries(vela-compositor-tests PRIVATE vela-pure GTest::gtest_main)
        include(GoogleTest)
        gtest_discover_tests(vela-compositor-tests TEST_PREFIX "compositor.")
    else()
        message(STATUS "GoogleTest not found: no compositor tests (package gtest)")
    endif()
endif()
