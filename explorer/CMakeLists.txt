# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# File Explorer (vela-files): Qt Quick without Qt Quick Controls, to start
# fast. It reuses shell pieces: installed apps ("Open with"), thumbnails, KDE
# service menus, system actions and the right-click menu component
# (MenuPanel.qml).
find_package(Qt6 6.5 REQUIRED COMPONENTS DBus Gui GuiPrivate Network Qml Quick QuickControls2 Svg)

qt_standard_project_setup(REQUIRES 6.5)

set(SHELL_SRC ${PROJECT_SOURCE_DIR}/shell/src)

# The Explorer's C++ code, shared by the app and its tests.
add_library(vela-files-core OBJECT
    src/fileops.cpp
    src/fileops.h
    src/foldermodel.cpp
    src/foldermodel.h
    src/places.cpp
    src/places.h
    ${SHELL_SRC}/appmodel.cpp
    ${SHELL_SRC}/appmodel.h
    ${SHELL_SRC}/filethumbnails.cpp
    ${SHELL_SRC}/filethumbnails.h
    ${SHELL_SRC}/iconprovider.h
    ${SHELL_SRC}/links.h
    ${SHELL_SRC}/servicemenus.cpp
    ${SHELL_SRC}/servicemenus.h
    ${SHELL_SRC}/systemactions.cpp
    ${SHELL_SRC}/systemactions.h)
target_include_directories(vela-files-core PUBLIC src ${SHELL_SRC})
target_compile_options(vela-files-core PRIVATE -Wall -Wextra)
target_link_libraries(vela-files-core PUBLIC
    Qt6::DBus
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::Svg)

qt_add_executable(vela-files src/main.cpp)

set(FILES_QML_FILES
    qml/Main.qml
    qml/TabPage.qml
    qml/NavPane.qml
    qml/AddressBar.qml
    qml/CommandBar.qml
    qml/DetailsView.qml
    qml/IconView.qml
    qml/HomePage.qml
    qml/ThisPcPage.qml
    qml/StatusBar.qml
    qml/MenuLayer.qml
    qml/ProgressPanel.qml
    qml/ToolButton.qml
    qml/FileIcon.qml
    qml/RenameField.qml
    qml/ScrollBar.qml
    qml/PreviewPane.qml
    ${PROJECT_SOURCE_DIR}/shell/qml/MenuPanel.qml)

foreach(file IN LISTS FILES_QML_FILES)
    get_filename_component(name ${file} NAME)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()

qt_add_qml_module(vela-files
    URI Vela.Files
    VERSION 1.0
    QML_FILES ${FILES_QML_FILES})

target_compile_options(vela-files PRIVATE -Wall -Wextra)
target_link_libraries(vela-files PRIVATE
    vela-files-core
    vela-controls # Theme, Style (common/)
    vela-controlsplugin
    vela-app) # the mouse's Back and Forward buttons (common/)

# The tests (ctest): copies and moves without a UI, with the cases that must
# never lose data (replace, disk full, type change).
if(VELA_BUILD_TOOLS)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    qt_add_executable(vela-files-test tests/transfers.cpp)
    target_compile_options(vela-files-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-files-test PRIVATE vela-files-core Qt6::Test)
    # Fault injection and process interruption at the atomic replacement's
    # syscall boundary, without adding test hooks to the application.
    target_link_options(vela-files-test PRIVATE "LINKER:--wrap=renameat2")
    add_test(NAME files-copies COMMAND vela-files-test)
    set_tests_properties(files-copies PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The views under Qt Quick Test (tests/tst_*.qml), on the QML in qml/:
    # recycled delegates take their state without a fade.
    find_package(Qt6 6.5 REQUIRED COMPONENTS QuickTest)
    qt_add_executable(vela-files-views-test tests/views_test.cpp)
    target_compile_definitions(vela-files-views-test PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_options(vela-files-views-test PRIVATE -Wall -Wextra)
    target_link_libraries(vela-files-views-test PRIVATE vela-files-core vela-controls vela-controlsplugin Qt6::QuickTest)
    add_test(NAME files-views COMMAND vela-files-views-test)
    set_tests_properties(files-views PROPERTIES TIMEOUT 120)
endif()

install(TARGETS vela-files RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES vela-files.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# Italian translation: strings in the code are in English, Italian lives
# in i18n/vela-files_it.ts and ends up in the executable (:/i18n/vela-files_it.qm, see
# shell/src/language.h). After changing texts:
#   cmake --build build --target update_translations
find_package(Qt6 6.7 REQUIRED COMPONENTS LinguistTools)
qt_add_translations(TARGETS vela-files SOURCE_TARGETS vela-files vela-files-core vela-controls
    TS_FILES i18n/vela-files_it.ts
    RESOURCE_PREFIX /i18n
    LUPDATE_TARGET vela-files_lupdate
    LRELEASE_TARGET vela-files_lrelease
    LUPDATE_OPTIONS -source-language en -target-language it -no-obsolete -locations none)
