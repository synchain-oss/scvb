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
  core/analysis_reanalysis_test.cpp
  core/test_hungarian.cpp
  core/test_assign.cpp
  core/test_balance.cpp
  core/test_authority.cpp
  core/test_version.cpp
  core/test_ipc_layout.cpp
  core/test_ipc_lifecycle.cpp
  core/analysis_coverage_test.cpp
  core/test_design_box.cpp
  core/test_state_codec.cpp
  core/test_state_features_roundtrip.cpp
  core/test_segment_edit.cpp
  core/test_audio_ring.cpp
  core/test_output_stage.cpp
  core/test_input_state.cpp
  core/test_input_session.cpp
  core/test_feat_ring_concurrency.cpp
  core/test_mix_source.cpp
  core/test_output_session.cpp
)

target_link_libraries(scvb_tests PRIVATE scvb_core Catch2::Catch2WithMain)
target_include_directories(scvb_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
target_compile_definitions(scvb_tests PRIVATE SCVB_TEST_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")

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

# scvb_params_tests:L0 参数层 golden 测试(链接 JUCE,离线 Catch2)。
# ADR-001 三主目标之外的辅助目标(J22);scvb_tests 保持 JUCE-free(ADR-011)。
# 03 §8 P-1/P-2/P-3:遍历 makeOutputLayout 的实际 getParameters() 对拍 tests/golden/params_v0.tsv。
add_executable(scvb_params_tests
  core/test_params_golden.cpp
  core/test_authority_params.cpp
  core/test_printer.cpp
  core/test_version_params.cpp
  core/test_segment_edit_service.cpp
  ${CMAKE_SOURCE_DIR}/src/output/OutputParams.cpp
  ${CMAKE_SOURCE_DIR}/src/output/OutputAuthority.cpp
  ${CMAKE_SOURCE_DIR}/src/output/AutomationPrinter.cpp
)

target_link_libraries(scvb_params_tests PRIVATE
  scvb_core
  Catch2::Catch2WithMain
  juce::juce_audio_processors
)

target_include_directories(scvb_params_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/output)
target_compile_definitions(scvb_params_tests PRIVATE SCVB_TEST_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")

if(MSVC)
  target_compile_options(scvb_params_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_params_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_params_tests COMMAND scvb_params_tests)

# scvb_plugin_common_tests:T26 WebView 装配层 L0 单测(链接 juce_gui_extra,离线 Catch2)。
# 只编可离线单测的 TU:ResourceProvider/BridgeBase/FallbackPanel/PlatformWebView。
# PlatformWebView.cpp 只含 makeWebViewOptions(纯 Options 装配);runtimeAvailable 已拆到
# PlatformWebViewRuntime.cpp(引用 WebView2 loader),与 WebViewHost(真 WebView2 实例化)一样
# 留待 T29/T30 接编辑器后真机验证,不编入本测试。
add_executable(scvb_plugin_common_tests
  webview/test_plugin_common.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/ResourceProvider.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/BridgeBase.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/FallbackPanel.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/PlatformWebView.cpp
)

target_link_libraries(scvb_plugin_common_tests PRIVATE
  Catch2::Catch2WithMain
  juce::juce_gui_extra
)

target_include_directories(scvb_plugin_common_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src/plugin-common
  ${CMAKE_SOURCE_DIR}/src/core
)

target_compile_definitions(scvb_plugin_common_tests PRIVATE
  JUCE_WEB_BROWSER=1
)

if(MSVC)
  target_compile_options(scvb_plugin_common_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_plugin_common_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_plugin_common_tests COMMAND scvb_plugin_common_tests)

# scvb_ipc_tests:L1 双进程 IPC 契约测试骨架(仅 WIN32;实现由 T07b 填充)。
if(SCVB_BUILD_IPC_TESTS AND WIN32)
  add_subdirectory(ipc)
endif()
