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
# web/ 的 UI 资源 -> BinaryData:resource provider 的唯一供给面,缺了就是空白窗口 + 看门狗超时
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ScvbWebAssets.cmake)
add_subdirectory(src/core)     # scvb_core
# T26:两插件共用的 WebView2 装配层(WebViewHost/PlatformWebView/ResourceProvider/FallbackPanel/BridgeBase)
add_subdirectory(src/plugin-common)  # scvb_plugin_common(INTERFACE,源编入各插件)
# T01:src/input + src/output 正式插件(T06 的 PR 恢复;S1 spike 核心已移植回 src/core/ipc/)
add_subdirectory(src/input)    # SCVBInput
add_subdirectory(src/output)   # SCVBOutput

if(SCVB_BUILD_TESTS)
  add_subdirectory(tests)      # scvb_tests + scvb_ipc_tests
endif()
if(SCVB_BUILD_TOOLS)
  add_subdirectory(tests/tools) # scvb_nulltest + scvb_diag(10-validation §4.2)
endif()
