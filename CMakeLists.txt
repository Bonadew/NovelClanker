cmake_minimum_required(VERSION 3.23)

project(NovelWriter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Widgets)
find_package(RocksDB REQUIRED)

function(novelwriter_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Novel storage (RocksDB). Kept free of Qt so it can be tested on its own.
add_library(novelstore STATIC
    src/NovelStore.cpp
    src/NovelStore.h
)
target_include_directories(novelstore PUBLIC src)
target_link_libraries(novelstore PRIVATE RocksDB::rocksdb)
novelwriter_warnings(novelstore)

# AI integration (LM Studio client, chat completions, prompts, AI settings). Qt Core/Network only, so it can be
# tested without widgets.
add_library(novelai STATIC
    src/AiSettings.cpp
    src/AiSettings.h
    src/LmChat.cpp
    src/LmChat.h
    src/LmStudioClient.cpp
    src/LmStudioClient.h
    src/PlanChat.cpp
    src/PlanChat.h
    src/PlanPrompt.cpp
    src/PlanStructure.cpp
    src/PlanStructure.h
    src/PlanPrompt.h
)
target_include_directories(novelai PUBLIC src)
target_link_libraries(novelai PUBLIC Qt6::Core Qt6::Network)
set_target_properties(novelai PROPERTIES AUTOMOC ON)
novelwriter_warnings(novelai)

# User interface. A library so tests can drive the real windows headlessly.
add_library(novelui STATIC
    src/MainWindow.cpp
    src/MainWindow.h
    src/MarkdownHighlighter.cpp
    src/MarkdownHighlighter.h
    src/ModelCombo.cpp
    src/ModelCombo.h
    src/NovelTree.cpp
    src/NovelTree.h
    src/PlanChatPanel.cpp
    src/PlanChatPanel.h
    src/SettingsDialog.cpp
    src/SettingsDialog.h
)
target_link_libraries(novelui PUBLIC novelstore novelai Qt6::Core Qt6::Gui Qt6::Widgets)
set_target_properties(novelui PROPERTIES AUTOMOC ON)
novelwriter_warnings(novelui)

add_executable(novelwriter WIN32 MACOSX_BUNDLE src/main.cpp)
target_link_libraries(novelwriter PRIVATE novelui)
novelwriter_warnings(novelwriter)

set_target_properties(novelwriter PROPERTIES
    OUTPUT_NAME "NovelWriter"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.novelwriter.app"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)

# --- Qt runtime deployment ------------------------------------------------------------------------------
# Qt is linked as shared libraries from the Conan cache. On Linux/macOS the build-tree RPATH already points
# there, so the executable runs as-is. Windows has no RPATH, so copy the Qt DLLs and plugins next to the
# executable after every build; it can then be started directly (double-click, IDE, debugger).
get_target_property(_qt_moc Qt6::moc IMPORTED_LOCATION)
get_filename_component(QT_BIN_DIR "${_qt_moc}" DIRECTORY)

if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}" NO_DEFAULT_PATH REQUIRED)
    set(_windeployqt_args
        --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw
        # TUIO touch input isn't needed. Note: windeployqt ignores --exclude-plugins when run with
        # --verbose 0, so keep the default verbosity.
        --exclude-plugins qtuiotouchplugin)

    add_custom_command(TARGET novelwriter POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}" ${_windeployqt_args} --no-compiler-runtime
                "$<TARGET_FILE:novelwriter>"
        COMMENT "Deploying Qt runtime next to $<TARGET_FILE_NAME:novelwriter>"
        VERBATIM)
endif()

# --- Installation (self-contained app folder / bundle) ---------------------------------------------------
# cmake --install build/Release --prefix dist
install(TARGETS novelwriter
    RUNTIME DESTINATION .
    BUNDLE DESTINATION .)

if(WIN32)
    # Ship the MSVC runtime DLLs so the folder runs on machines without the VC++ redistributable.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    if(MSVC_VERSION LESS 1950 OR CMAKE_VERSION VERSION_GREATER_EQUAL 4.2)
        include(InstallRequiredSystemLibraries)
    endif()
    if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        # Older CMake doesn't know newer Visual Studio versions; find the CRT next to the compiler instead
        # (<VC>/Tools/MSVC/<ver>/bin/Host*/<arch>/cl.exe -> <VC>/Redist/MSVC/<ver>/<arch>/Microsoft.VC*.CRT).
        cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH _vc_dir)
        foreach(_i RANGE 1 6)
            cmake_path(GET _vc_dir PARENT_PATH _vc_dir)
        endforeach()
        file(GLOB _crt_dirs LIST_DIRECTORIES true "${_vc_dir}/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
        if(_crt_dirs)
            list(SORT _crt_dirs COMPARE NATURAL ORDER DESCENDING)
            list(GET _crt_dirs 0 _crt_dir)
            file(GLOB CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "${_crt_dir}/*.dll")
        endif()
    endif()
    if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        message(WARNING "MSVC runtime DLLs not found; 'cmake --install' output will need the VC++ redistributable")
    endif()
    install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION .)

    install(CODE "
        execute_process(
            COMMAND \"${WINDEPLOYQT_EXECUTABLE}\" ${_windeployqt_args} --no-compiler-runtime
                    \"\${CMAKE_INSTALL_PREFIX}/$<TARGET_FILE_NAME:novelwriter>\"
            COMMAND_ERROR_IS_FATAL ANY)
    ")
elseif(APPLE)
    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt HINTS "${QT_BIN_DIR}" NO_DEFAULT_PATH REQUIRED)
    install(CODE "
        execute_process(
            COMMAND \"${MACDEPLOYQT_EXECUTABLE}\" \"\${CMAKE_INSTALL_PREFIX}/NovelWriter.app\"
            COMMAND_ERROR_IS_FATAL ANY)
    ")
endif()

# --- Tests ----------------------------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_executable(novelstore_test tests/NovelStoreTest.cpp)
    target_link_libraries(novelstore_test PRIVATE novelstore RocksDB::rocksdb) # RocksDB: writes old formats
    novelwriter_warnings(novelstore_test)
    add_test(NAME novelstore COMMAND novelstore_test "${CMAKE_CURRENT_BINARY_DIR}/test-data")

    add_executable(lmstudio_test tests/LmStudioClientTest.cpp)
    target_link_libraries(lmstudio_test PRIVATE novelai)
    novelwriter_warnings(lmstudio_test)
    add_test(NAME lmstudio COMMAND lmstudio_test)

    add_executable(settings_test tests/SettingsDialogTest.cpp)
    target_link_libraries(settings_test PRIVATE novelui)
    novelwriter_warnings(settings_test)
    add_test(NAME settings COMMAND settings_test)

    add_executable(chat_test tests/ChatCompletionTest.cpp)
    target_link_libraries(chat_test PRIVATE novelai)
    novelwriter_warnings(chat_test)
    add_test(NAME chat COMMAND chat_test)

    add_executable(planstructure_test tests/PlanStructureTest.cpp)
    target_link_libraries(planstructure_test PRIVATE novelai)
    novelwriter_warnings(planstructure_test)
    add_test(NAME planstructure COMMAND planstructure_test)

    add_executable(mainwindow_test tests/MainWindowTest.cpp)
    target_link_libraries(mainwindow_test PRIVATE novelui)
    novelwriter_warnings(mainwindow_test)
    add_test(NAME mainwindow COMMAND mainwindow_test "${CMAKE_CURRENT_BINARY_DIR}/test-data-ui")

    # Widget tests run headless: Qt's "minimal" platform never shows a window.
    set_tests_properties(settings mainwindow PROPERTIES TIMEOUT 120 ENVIRONMENT
        "QT_QPA_PLATFORM=minimal;QT_PLUGIN_PATH=${QT_BIN_DIR}/../plugins")
    if(WIN32)
        # Find the Qt DLLs without relying on windeployqt having run.
        set_tests_properties(lmstudio settings chat planstructure mainwindow PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR}")
    endif()
endif()
