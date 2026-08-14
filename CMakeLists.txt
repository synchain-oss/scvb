cmake_minimum_required(VERSION 3.22)

# 静态 CRT 必须在 project() 之前 —— 保证 /MT over /MD(对齐 Bridge / WebView2 静态 loader)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(SCVB VERSION 0.1.0)

# ------------------------------------------------------------------
# JUCE 8 本地检出(版本单一真源 = 仓库根 .juce-version)
# ------------------------------------------------------------------
set(JUCE_PATH "" CACHE PATH "Path to JUCE root directory")

if(NOT JUCE_PATH)
  message(FATAL_ERROR "JUCE_PATH must be set. Example: -DJUCE_PATH=C:/path/to/JUCE")
endif()

add_subdirectory(${JUCE_PATH} juce)

# ------------------------------------------------------------------
# WebView2 NuGet package (Windows only)
# JUCE's NEEDS_WEBVIEW2 only *searches + links* an already-present NuGet
# package (it does NOT download). We fetch it at configure time into a
# deterministic dir and point JUCE_WEBVIEW2_PACKAGE_LOCATION at it, so the
# build is reproducible locally and in CI (no dependency on %USERPROFILE%).
# ------------------------------------------------------------------
if(WIN32)
  set(WEBVIEW2_VERSION "1.0.2957.106" CACHE STRING "Microsoft.Web.WebView2 NuGet version")
  set(WEBVIEW2_PKG_DIR "${CMAKE_BINARY_DIR}/packages")

  # Skip download if the package is already present (offline / cached CI)
  file(GLOB _wv2_existing "${WEBVIEW2_PKG_DIR}/Microsoft.Web.WebView2*")
  if(NOT _wv2_existing)
    find_program(NUGET_EXE nuget) # windows runners ship nuget.exe
    if(NOT NUGET_EXE)
      message(FATAL_ERROR
        "nuget.exe not found. Install the NuGet CLI or pre-populate ${WEBVIEW2_PKG_DIR}.")
    endif()
    message(STATUS "Fetching Microsoft.Web.WebView2 ${WEBVIEW2_VERSION} via NuGet ...")
    execute_process(
      COMMAND "${NUGET_EXE}" install Microsoft.Web.WebView2
              -Version ${WEBVIEW2_VERSION}
              -OutputDirectory "${WEBVIEW2_PKG_DIR}"
              -ExcludeVersion # fixed dir name: Microsoft.Web.WebView2 (no version suffix)
      RESULT_VARIABLE _nuget_rc)
    if(NOT _nuget_rc EQUAL 0)
      message(FATAL_ERROR "nuget install Microsoft.Web.WebView2 failed (rc=${_nuget_rc}).")
    endif()
  endif()

  # Make JUCE's FindWebView2 look here (overrides the default %USERPROFILE% path)
  set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${WEBVIEW2_PKG_DIR}" CACHE PATH "" FORCE)
endif()

# ------------------------------------------------------------------
# 构建选项
# ------------------------------------------------------------------
option(SCVB_BUILD_TESTS "Build Catch2 unit tests (scvb_tests)" ON)
option(SCVB_BUILD_IPC_TESTS "Build L1 IPC contract tests (scvb_ipc_tests, Windows only)" ON)
option(SCVB_BUILD_TOOLS "Build validation console tools (tests/tools: scvb_nulltest)" ON)
option(SCVB_TESTS_WITH_EBUR128 "Build [reference] K-weighting tests against libebur128 (needs network)" OFF)

# ------------------------------------------------------------------
# 目标(ADR-001/012:scvb_core + 两个插件 + 测试)
# ------------------------------------------------------------------
include(CTest)               # enable_testing() 于顶层,让根 CTestTestfile.cmake 生成并 subdirs() 进 tests/
add_subdirectory(src/core)     # scvb_core
# T02:S1 路由 spike(T01 的 src/input + src/output 空壳暂不构建,由 spikes/s1 提供两个 VST3;
#      T06 的 PR 会把 spike 核心移植回 src/core/ipc/ 并恢复 src/input/src/output 正式插件)
add_subdirectory(spikes/s1)    # s1_core + SCVBInput + SCVBOutput
# T04:S3 WebView 承载力 spike(T04 的 Output 最小插件壳 + WebView2 编辑器;由 T26 的 PR 删除)
add_subdirectory(spikes/s3)    # SCVBS3Output
# T05:S4 状态体量 spike(T05 的满配 state 编解码 + 验收跑分;由 T19 的 PR 删除)
add_subdirectory(spikes/s4)    # s4_state + scvb_s4_state
if(SCVB_BUILD_TESTS)
  add_subdirectory(tests)      # scvb_tests + scvb_ipc_tests + scvb_s1_tests
endif()
if(SCVB_BUILD_TOOLS)
  add_subdirectory(tests/tools) # scvb_nulltest + scvb_diag(10-validation §4.2)
endif()
