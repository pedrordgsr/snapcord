cmake_minimum_required(VERSION 3.28)

# On Windows, link the vcpkg dependencies statically (fewer DLLs to ship) while keeping the dynamic CRT that Qt uses.
if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
endif()

project(Snapcord
    VERSION 0.3.0
    DESCRIPTION "Native, lightweight, voice-focused Discord client"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg Network WebSockets Multimedia MultimediaWidgets LinguistTools)
qt_standard_project_setup()

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Opus CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(unofficial-nayuki-qr-code-generator CONFIG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
include(cmake/Dependencies.cmake)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_subdirectory(src/core)
add_subdirectory(src/voice)
add_subdirectory(src/platform)
add_subdirectory(src/app)

# Developer tool that regenerates the packaging icons from the SVG (not built by default).
qt_add_executable(snapcord_render_icons tools/render_icons.cpp)
target_link_libraries(snapcord_render_icons PRIVATE Qt6::Gui Qt6::Svg)
set_target_properties(snapcord_render_icons PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF EXCLUDE_FROM_ALL ON)

# Promo video (motion graphics + synthesized soundtrack), encoded with Media Foundation. Not part of the build.
if(WIN32)
    qt_add_executable(snapcord_promo tools/promo_video.cpp)
    target_link_libraries(snapcord_promo PRIVATE Qt6::Gui mfplat mfreadwrite mfuuid ole32)
    set_target_properties(snapcord_promo PROPERTIES WIN32_EXECUTABLE OFF EXCLUDE_FROM_ALL ON)
endif()

option(SNAPCORD_BUILD_TESTS "Build the unit tests" ON)
if(SNAPCORD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
