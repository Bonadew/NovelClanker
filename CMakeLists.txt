cmake_minimum_required(VERSION 3.23)

project(NovelWriter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_executable(novelwriter WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/MarkdownHighlighter.cpp
    src/MarkdownHighlighter.h
)

target_link_libraries(novelwriter PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

if(MSVC)
    target_compile_options(novelwriter PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(novelwriter PRIVATE -Wall -Wextra -Wpedantic)
endif()

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
        # qtuiotouchplugin would drag in Qt Network plus its TLS/network plugins. Note: windeployqt ignores
        # --exclude-plugins when run with --verbose 0, so keep the default verbosity.
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
