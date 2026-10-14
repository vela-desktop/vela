# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE VELA_REPORT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
# Source RPM archives intentionally contain no .git directory; preserve the
# exact selected commit passed by the authenticated packaging workflow/updater.
if(DEFINED VELA_PACKAGE_COMMIT AND NOT VELA_PACKAGE_COMMIT STREQUAL "")
    set(VELA_REPORT_COMMIT "${VELA_PACKAGE_COMMIT}")
elseif(NOT VELA_REPORT_COMMIT)
    set(VELA_REPORT_COMMIT "unknown")
endif()
execute_process(COMMAND git status --porcelain --untracked-files=normal
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE VELA_REPORT_CHANGES ERROR_QUIET)
set(VELA_REPORT_DIRTY "false")
if(VELA_REPORT_CHANGES)
    set(VELA_REPORT_DIRTY "true")
endif()
configure_file(vela-report.in vela-report @ONLY)
file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/vela-report PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(GLOB VELA_REPORT_MODULES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vela_report/*.py)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${VELA_REPORT_MODULES})
file(COPY vela_report DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
    PATTERN "__pycache__" EXCLUDE)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/vela-report DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY vela_report DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/report
    PATTERN "__pycache__" EXCLUDE)

option(VELA_REPORT_GUI "Build the standalone report GUI (Qt Widgets)" ${VELA_BUILD_SHELL})
if(VELA_REPORT_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
    add_executable(vela-report-gui gui/main.cpp)
    target_link_libraries(vela-report-gui PRIVATE Qt6::Widgets)
    target_compile_options(vela-report-gui PRIVATE -Wall -Wextra)
    install(TARGETS vela-report-gui RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
    install(FILES vela-report.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()
if(VELA_BUILD_TOOLS)
    add_test(NAME reporting COMMAND ${Python3_EXECUTABLE} -m unittest discover
        -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -v)
    set_tests_properties(reporting PROPERTIES
        ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT 60)
    if(VELA_REPORT_GUI)
        add_test(NAME reporting-gui COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/gui_smoke.py $<TARGET_FILE:vela-report-gui>)
        set_tests_properties(reporting-gui PROPERTIES TIMEOUT 30)
    endif()
endif()
