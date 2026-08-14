# Catch2 冒烟(scvb_tests)+ L1 IPC 契约骨架(scvb_ipc_tests,仅 WIN32)。
# 目标结构真源:10-validation §2.1(本卡只落 scvb_tests 冒烟 + scvb_ipc_tests 骨架)。
enable_testing()

include(FetchContent)

# Catch2 仅测试目标依赖(不进插件运行时),版本钉死避免上游漂移(06 §3.1 / ADR-011)。
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.4
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

# [reference] 对拍依赖(仅测试;不进插件运行时,ADR-009)。默认关闭,启用需网络。
# libebur128 默认 BUILD_SHARED_LIBS=ON,须显式关掉以静态链接(与 /MT 静态 CRT 对齐)。
if(SCVB_TESTS_WITH_EBUR128)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    libebur128
    GIT_REPOSITORY https://github.com/jiixyj/libebur128.git
    GIT_TAG v1.2.6
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(libebur128)
endif()

# scvb_tests:L0 冒烟,仅链接 scvb_core(不依赖 JUCE,ADR-001/011)。
add_executable(scvb_tests
  core/test_smoke.cpp
  core/test_kweighting.cpp
  core/test_features.cpp
  core/test_pancurve.cpp
  core/test_transition.cpp
  core/test_loudness.cpp
  core/test_vad.cpp
  core/test_segmentation.cpp
  core/test_hungarian.cpp
  core/test_assign.cpp
)

target_link_libraries(scvb_tests PRIVATE scvb_core Catch2::Catch2WithMain)
target_include_directories(scvb_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/core)

if(SCVB_TESTS_WITH_EBUR128)
  target_compile_definitions(scvb_tests PRIVATE SCVB_TESTS_WITH_EBUR128)
  target_include_directories(scvb_tests PRIVATE ${libebur128_SOURCE_DIR}/ebur128)
  target_link_libraries(scvb_tests PRIVATE ebur128)
endif()

if(MSVC)
  target_compile_options(scvb_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_tests COMMAND scvb_tests)

# scvb_ipc_tests:L1 双进程 IPC 契约测试骨架(仅 WIN32;实现由 T07b 填充)。
if(SCVB_BUILD_IPC_TESTS AND WIN32)
  add_subdirectory(ipc)
endif()

# scvb_s1_tests:S1 路由 spike 离线验证(仅 WIN32;链接 spikes/s1 的 s1_core)。
if(WIN32)
  add_subdirectory(s1)
endif()
