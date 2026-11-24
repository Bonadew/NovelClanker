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
