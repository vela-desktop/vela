# Vela targets modern x86-64 hardware (roughly 2017 and newer). The supported
# binary baseline is x86-64-v3, independently of the Vulkan 1.4 GPU requirement.
# An explicit local override is useful for repeatable CPU benchmarks or for
# optimizing a locally built installation (for example -march=znver5).
# Below-v3 overrides are unsupported except for controlled A/B measurements.
set(VELA_CPU_TARGET "x86-64-v3" CACHE STRING
    "x86-64 -march target (minimum supported: x86-64-v3; local: native, znver5)")
set_property(CACHE VELA_CPU_TARGET PROPERTY STRINGS x86-64-v3 x86-64-v4 native znver5)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    if(VELA_CPU_TARGET STREQUAL "")
        message(FATAL_ERROR "VELA_CPU_TARGET must not be empty on x86-64")
    endif()
    include(CheckCCompilerFlag)
    include(CheckCXXCompilerFlag)
    string(MAKE_C_IDENTIFIER "${VELA_CPU_TARGET}" VELA_CPU_TARGET_IDENTIFIER)
    set(VELA_C_MARCH_TEST "VELA_C_MARCH_${VELA_CPU_TARGET_IDENTIFIER}")
    set(VELA_CXX_MARCH_TEST "VELA_CXX_MARCH_${VELA_CPU_TARGET_IDENTIFIER}")
    check_c_compiler_flag("-march=${VELA_CPU_TARGET}" ${VELA_C_MARCH_TEST})
    check_cxx_compiler_flag("-march=${VELA_CPU_TARGET}" ${VELA_CXX_MARCH_TEST})
    # Check the result *variables*; expanding their values can leave an invalid
    # if(NOT OR NOT) when one or both compiler flag probes fail.
    if(NOT ${VELA_C_MARCH_TEST} OR NOT ${VELA_CXX_MARCH_TEST})
        message(FATAL_ERROR
            "Compiler does not support VELA_CPU_TARGET='${VELA_CPU_TARGET}' for both C and C++")
    endif()
    # Applied after CMake's inherited CFLAGS/CXXFLAGS so the intended target
    # is effective even on distributions whose defaults contain -march=x86-64.
    # Only Vela's compiled code is affected, not Qt, wlroots, Mesa or shaders.
    add_compile_options("-march=${VELA_CPU_TARGET}")
    message(STATUS "Vela x86-64 code-generation target: ${VELA_CPU_TARGET}")
else()
    message(STATUS "VELA_CPU_TARGET is x86-64-only; no CPU ISA flag on this target")
endif()
