cmake_minimum_required(VERSION 3.16)
project(lunar_de VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET
    xcb
    xcb-util
    xcb-ewmh
    xcb-icccm
    xcb-keysyms
    xcb-randr
)
pkg_check_modules(GFX REQUIRED IMPORTED_TARGET
    libjpeg
    libpng
    freetype2
    fontconfig
)

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(lunar-wm
    src/wm/main.cpp
    src/wm/window_manager.cpp
    src/common/image_utils.cpp
    src/common/desktop_apps.cpp
)
target_include_directories(lunar-wm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lunar-wm PRIVATE PkgConfig::XCB PkgConfig::GFX dl)

add_executable(lunar-panel
    src/panel/main.cpp
    src/panel/panel.cpp
    src/common/image_utils.cpp
    src/common/desktop_apps.cpp
)
target_include_directories(lunar-panel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lunar-panel PRIVATE PkgConfig::XCB PkgConfig::GFX dl)
