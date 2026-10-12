# scvb_auhost —— 二进制级调度宿主(B 线 M13)。独立 CMake 工程。
#
# **不进根工程**:根 CMakeLists.txt / tests/CMakeLists.txt 都不 add_subdirectory 本目录,Windows 构建与
# gates 零影响。只在 .github/workflows/macos-oop.yml 的 scvb-auhost job 里单独 configure:
#   cmake -S tests/macos/auhost -B build-auhost -DCMAKE_BUILD_TYPE=Release
# 不依赖 JUCE:宿主只用 AudioToolbox / CoreFoundation 加载**已构建好的** SCVB component(取自同一个
# run 里 macos-build 上传的 artifact),所以这里不编任何插件。
#
# 复用的产品 / 测试源码(只读,路径相对仓库根):
#   src/core/state/StateCodec.cpp、InputStateCodec.cpp —— 给 Input 写通道号用的 state 容器编码,
#     与插件解码是同一份实现(写错格式时插件会整块拒载,宿主读回核对当场红);StateCodec.cpp 的 CRVS
#     解码引用 src/core/analysis/PanCurve.cpp 的 isPanCurvePointUsable,所以它也编进来(纯 C++,无依赖);
#   src/core/ipc/SegmentLayout.h、Registry.h —— registry 段布局(只读映射,取 pid / connected_mask);
#   tests/support/presence_meter.h —— A 线 A-1 的逐轨在场判据。
# 本目录的 auhost_engine.h / auhost_judge.h 只用标准库,自测(scvb_auhost --self-test)可在任何平台编译。
cmake_minimum_required(VERSION 3.22)

# 必须在 project() 之前(参与编译器探测);与根工程 / spike 同写法。
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architecture")
endif()
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(SCVBAuHost VERSION 0.0.1 LANGUAGES CXX)

if(NOT APPLE)
  message(FATAL_ERROR "tests/macos/auhost is macOS-only (AudioToolbox / AUHostingService).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(SCVB_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_executable(scvb_auhost
  scvb_auhost.cpp
  ${SCVB_REPO_ROOT}/src/core/state/StateCodec.cpp
  ${SCVB_REPO_ROOT}/src/core/state/InputStateCodec.cpp
  ${SCVB_REPO_ROOT}/src/core/analysis/PanCurve.cpp)
target_include_directories(scvb_auhost PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SCVB_REPO_ROOT}/src/core
  ${SCVB_REPO_ROOT}/tests)
# 与 build-macos.yml 的 first-party 零警告门同一个态度:警告即错误。-fblocks:AudioComponentInstantiate 的回调。
target_compile_options(scvb_auhost PRIVATE -fblocks -Wall -Wextra -Werror)
target_link_libraries(scvb_auhost PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
