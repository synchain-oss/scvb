# scvb_pagezoom_probe —— 真 WebKit 上量 WKWebView.pageZoom 的坐标系。独立 CMake 工程。
#
# **不进根工程**:根 CMakeLists.txt / tests/CMakeLists.txt 都不 add_subdirectory 本目录,Windows 构建与 gates 零影响。
# 只在 .github/workflows/build-macos.yml 的探针那一步里单独 configure(不依赖 JUCE,也不依赖插件构建):
#   cmake -S tests/macos/pagezoom-probe -B build-pagezoom -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.22)

# 必须在 project() 之前(参与编译器探测);与根工程 / auhost 同写法。
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architecture")
endif()
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(SCVBPageZoomProbe VERSION 0.0.1 LANGUAGES OBJCXX)

if(NOT APPLE)
  message(FATAL_ERROR "tests/macos/pagezoom-probe is macOS-only (WKWebView).")
endif()

set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

add_executable(scvb_pagezoom_probe pagezoom_probe.mm)
target_compile_options(scvb_pagezoom_probe PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(scvb_pagezoom_probe PRIVATE "-framework Cocoa" "-framework WebKit")
