cmake_minimum_required(VERSION 3.22)

# 静态 CRT 必须在 project() 之前 —— 保证 /MT over /MD(对齐 Bridge / WebView2 静态 loader)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# macOS 目标(必须在 project() 之前:两者参与编译器探测)。非 Apple 平台完全忽略这两个变量,
# Windows 零影响。project() 之前 APPLE 还没定义,所以用 NOT DEFINED 守卫(命令行 -D 可覆盖)。
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  # 只出 Apple Silicon。universal 不做:juceaide 与全部 JUCE 模块编译量翻倍。保留为 cache
  # 变量,哪天要出 universal 只改这一个参数。
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS target architecture")
endif()
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  # 11.0 = 首个支持 Apple Silicon 的系统(Big Sur),与 Bridge 同下限。不显式设就取构建机
  # SDK 的版本,产物在老系统上会拒绝加载 —— mac 移植最常见的静默事故。
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(SCVB VERSION 0.9.0)

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
# 三个插件共用的格式与参数 ID 口径([B 线 M06b];三个 juce_add_plugin 与 scvb_params_tests 只读这里)
# ------------------------------------------------------------------
# 格式:先 VST3,Apple 上再追加 AU。显式按平台分支,不靠「JUCE 在非 Apple 平台静默滤掉 AU」
# 这一未文档化行为 —— 哪天 JUCE 改成报错或告警,Windows 构建不该跟着变。Windows 上本变量就是
# VST3 一个值,与改动前的 FORMATS 相同(仍恰好 3 个 .vst3)。
# 普通变量而不是 cache:产物形态只有这一个真源,不给命令行 -D 留第二个入口。
set(SCVB_PLUGIN_FORMATS VST3)
if(APPLE)
  list(APPEND SCVB_PLUGIN_FORMATS AU)
endif()
message(STATUS "SCVB plugin formats: ${SCVB_PLUGIN_FORMATS}")

# 参数 ID 的两个 JUCE 宏,显式钉住(值 = JUCE 8.0.8 的默认值,见 juce_audio_plugin_client.h:89-116,
# 所以 Windows 二进制与 VST3 参数 ID 不变)。钉住的理由:它们决定宿主工程里保存的自动化 ID ——
#   JUCE_FORCE_USE_LEGACY_PARAM_IDS=0         ID 由 ParamID 字符串散列得来,而不是参数下标;
#   JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS=1 散列值清掉最高位(VST3 与 AU 两个 wrapper 同一规则)。
# 任何一个翻转,已发布工程里的自动化就全部对不上。插件目标以 PUBLIC 传给各格式 wrapper;
# scvb_params_tests 读同一个变量,tests/core/test_au_param_ids.cpp 用 static_assert 钉值、
# 并对 123 个 ParamID 现算 AU 参数 ID 与文件内嵌的冻结表逐项对拍。
# Input / Monitor 目前没有自动化参数,两个宏对它们暂不起作用;三个插件仍统一挂上是**预防性**的 ——
# 将来哪个插件加了参数,ID 口径已经钉好,不会悄悄落回 JUCE 默认。不要当成冗余删掉:
# mac CI(build-macos.yml)的「宏进 wrapper」一步逐个插件核这两个 -D,哪个插件删了都会在那一步红。
set(SCVB_PLUGIN_PARAM_ID_DEFINITIONS
  JUCE_FORCE_USE_LEGACY_PARAM_IDS=0
  JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS=1
)

# ------------------------------------------------------------------
# 目标(ADR-001/012:scvb_core + 三个插件 + 测试;第三主目标 SCVBMonitor 由 [J75] 授权)
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
# T45 [J75]:第三主目标 —— 只读监视器(0 自动化参数 / 音频直通 / 只读 attach viz 段)
add_subdirectory(src/monitor)  # SCVBMonitor

if(SCVB_BUILD_TESTS)
  add_subdirectory(tests)      # scvb_tests + scvb_ipc_tests
endif()
if(SCVB_BUILD_TOOLS)
  add_subdirectory(tests/tools) # scvb_nulltest + scvb_diag(10-validation §4.2)
endif()
