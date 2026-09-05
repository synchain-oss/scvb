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
  core/test_segment_diff.cpp
  core/test_analysis_pipeline.cpp
  core/test_design_box.cpp
  core/test_state_codec.cpp
  core/test_state_features_roundtrip.cpp
  core/test_features_snapshot.cpp
  core/test_segment_edit.cpp
  core/test_audio_ring.cpp
  core/test_output_stage.cpp
  core/test_input_state.cpp
  core/test_input_session.cpp
  core/test_input_bridge_ipc.cpp
  core/test_feat_ring_concurrency.cpp
  core/test_mix_source.cpp
  core/test_output_session.cpp
  core/test_feature_fingerprint.cpp
  core/test_suggestion_export.cpp
  core/test_viz_plane.cpp
  # 隐藏用例([.][vizcost]):viz 升频(SL-192)的单帧耗时实测面。ctest 默认不跑 ——
  # 门禁里不该有按墙钟判红的断言。手工 `scvb_tests.exe "[vizcost]"` 复现。
  core/test_viz_publish_cost.cpp
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

# ─────────────────────────────────────────────────────────────────────────────
# [SL-320] **四套快套件的显式上界**(scvb_tests / scvb_params_tests /
# scvb_plugin_common_tests / scvb_input_bridge_tests)。本段是这四套的口径真源,
# 另外三处只留一行属性 + 指回这里 —— 同一份事实抄四处,改一处就是三句假注释。
#
# 病灶:上界此前**一分为二**。`scripts/gates.ps1` 的 ctest 命令行带 `--timeout 300`,
# 只有本机吃得到;而**出包硬门** `.github/workflows/build-vst3.yml` 的 ctest **不带**
# `--timeout`,这四套在那条路上吃的是 CTest 默认的 **1500s**。同一个「上界」两个真源、
# 差 5 倍:本机 300s 就拦下的挂死,出包那条路要占住 runner 25 分钟。
# 属性跟着**测试**走,不跟着**谁来跑**走 —— 写进这里,两边同吃一份。
#
# **观测**(全部实测,`Passed` 那一列;本机 = 开发机空载连跑 5 轮,CI = build-and-validate):
#     套件                        本机 5 轮(s)                        CI 两轮(s)
#     scvb_tests                  2.14 2.14 2.05 2.10 2.06            2.57 2.52
#     scvb_params_tests           0.27 0.30 0.26 0.25 0.28            0.27 0.33
#     scvb_plugin_common_tests    0.19 0.19 0.17 0.25 0.19            0.13 0.12
#     scvb_input_bridge_tests     0.08 0.04 0.03 0.03 0.04            0.01 0.01
# 跨两侧最慢的一次:2.57 / 0.33 / 0.25 / 0.08。
#
# **为什么四套同取 30s,而不是「稳态 × 3」**:这四套是**亚秒级**的,× 3 会得到
# 7.7 / 1.0 / 0.75 / 0.24 秒 —— 那不是「拦挂死」的线,是**性能预算**,而且紧到一次冷启动、
# 一次杀毒扫描、一次 runner 被抢就会把**正常通过**的跑判成 Timeout,让出包硬门常红。
# 本文件下面 `scvb_host_tests` 那段已经写过同一句话:「拦挂死不等于 police 慢」。
# 所以规则是 **`max(最慢观测 × 3, 30s)`**,今天四套都落在 30s 这一档:
#   · 对最慢的 `scvb_tests`(2.57s)是 ~12 倍余量,对另外三套是 100 倍以上;
#   · 又比它取代的 1500s 小 50 倍 —— 真挂死半分钟内拦下,不再占住机器 25 分钟;
#   · 排序也合理:30 < 600(monitor,实测 16s)< 900(ipc / host,host 实测 223–372s)。
#     ⚠ **全景在这里补齐一次**:`scvb_ipc_tests` 的 900 在 `tests/ipc/CMakeLists.txt`,
#     **不在本文件**(由 `SCVB_BUILD_IPC_TESTS` 控制,默认 ON,本机与 CI 都真的跑)。
#     本段自称「四套的口径真源」,而读者会拿它当**七套的全清单**去推「谁还落在兜底上」——
#     本卡第 1 轮复审就是这么栽的:只 grep 了本文件、没扫 `tests/**`,把 ipc 说成没有属性。
#     要问「谁有属性」,扫全树,或直接读生成物 `build*/**/CTestTestfile.cmake` ——
#     那才是 ctest 真正吃的那份。本 PR 之后**七套全部有属性**,`--timeout` 一套都管不到。
# 哪天某一套真的变慢到逼近 30s,**先去查它为什么变慢**,不要顺手抬这个数 ——
# 抬数是把信号调低,和 host 那段的告诫同源。
#
# ⚠ 与 `--timeout` 的关系:命令行的 `--timeout` 只对**没有 TIMEOUT 属性**的测试生效,
# 有属性的以属性为准(gates.ps1 gate 6 那段有实测记录)。所以这四套从此**不再受**
# 任何一侧命令行开关的影响;命令行那个数退化为「新加的、还没写属性的测试」的兜底。
# ─────────────────────────────────────────────────────────────────────────────
set_tests_properties(scvb_tests PROPERTIES TIMEOUT 30)

# scvb_params_tests:L0 参数层 golden 测试(链接 JUCE,离线 Catch2)。
# ADR-001 三主目标之外的辅助目标(J22);scvb_tests 保持 JUCE-free(ADR-011)。
# 03 §8 P-1/P-2/P-3:遍历 makeOutputLayout 的实际 getParameters() 对拍 tests/golden/params_v0.tsv。
add_executable(scvb_params_tests
  core/test_params_golden.cpp
  core/test_authority_params.cpp
  core/test_printer.cpp
  core/test_version_params.cpp
  core/test_segment_edit_service.cpp
  core/test_bridge_args.cpp
  core/test_ui_defaults_store.cpp
  ${CMAKE_SOURCE_DIR}/src/output/OutputParams.cpp
  ${CMAKE_SOURCE_DIR}/src/output/OutputAuthority.cpp
  ${CMAKE_SOURCE_DIR}/src/output/AutomationPrinter.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/UiDefaultsStore.cpp
)

target_link_libraries(scvb_params_tests PRIVATE
  scvb_core
  Catch2::Catch2WithMain
  juce::juce_audio_processors
)

target_include_directories(scvb_params_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src/output
  ${CMAKE_SOURCE_DIR}/src/plugin-common   # BridgeBase.h(UiDefaultsStore 取缩放档位边界)
)
target_compile_definitions(scvb_params_tests PRIVATE SCVB_TEST_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")

if(MSVC)
  target_compile_options(scvb_params_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_params_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_params_tests COMMAND scvb_params_tests)
# [SL-320] 上界与理由见上面 `scvb_tests` 那段(四套同一份口径,不在这里抄一遍)。
set_tests_properties(scvb_params_tests PROPERTIES TIMEOUT 30)

# scvb_monitor_tests:T45 Monitor 三铁律的 L0 断言(0 参数 / 直通按位相等 / 共享段零写入)。
# 只编 MonitorProcessor.cpp,并定义 SCVB_MONITOR_HEADLESS —— MonitorEditor 依赖 WebViewHost 的
# 真 WebView2 实例化,与 InputEditor/OutputEditor 同款留给 gate 8 的真机 GUI pluginval。
add_executable(scvb_monitor_tests
  core/test_monitor_processor.cpp
  # [统筹加码] 全数据链双进程 harness:真 VizPublisher 对端进程 + 真 MonitorProcessor,
  # 覆盖 发布→读到→组切换→退出空态→重连,以及写方停摆判陈旧。
  core/test_monitor_harness.cpp
  ${CMAKE_SOURCE_DIR}/src/monitor/MonitorProcessor.cpp
)

target_link_libraries(scvb_monitor_tests PRIVATE
  scvb_core
  Catch2::Catch2WithMain
  juce::juce_audio_processors
)

target_include_directories(scvb_monitor_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/monitor ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(scvb_monitor_tests PRIVATE SCVB_MONITOR_HEADLESS=1)

if(MSVC)
  target_compile_options(scvb_monitor_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_monitor_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_monitor_tests COMMAND scvb_monitor_tests)
# harness 拉真进程(scvb_ipc_peer),给足超时。对端的构建依赖在 tests/tools/CMakeLists.txt 里挂 ——
# 顶层是先 add_subdirectory(tests) 再 add_subdirectory(tests/tools),此处 scvb_ipc_peer 还不存在。
set_tests_properties(scvb_monitor_tests PROPERTIES TIMEOUT 600)

# scvb_plugin_common_tests:T26 WebView 装配层 L0 单测(链接 juce_gui_extra,离线 Catch2)。
# 只编可离线单测的 TU:ResourceProvider/BridgeBase/FallbackPanel/PlatformWebView。
# PlatformWebView.cpp 只含 makeWebViewOptions(纯 Options 装配);runtimeAvailable 已拆到
# PlatformWebViewRuntime.cpp(引用 WebView2 loader),与 WebViewHost(真 WebView2 实例化)一样
# 留待 T29/T30 接编辑器后真机验证,不编入本测试。
add_executable(scvb_plugin_common_tests
  webview/test_plugin_common.cpp
  webview/test_web_assets_embedded.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/ResourceProvider.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/BridgeBase.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/FallbackPanel.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/PlatformWebView.cpp
)

target_link_libraries(scvb_plugin_common_tests PRIVATE
  Catch2::Catch2WithMain
  juce::juce_gui_extra
  juce::juce_audio_processors   # 只为 WebViewHost.h 的 include 面(取其 constexpr 常量,不编 .cpp)
  # 三个插件真实生成的 BinaryData —— test_web_assets_embedded 拿它们喂真 ResourceProvider。
  # 链接的是**产物**而不是复刻一份 glob 规则:cmake 那边改了范围,这里立刻跟着变。
  SCVBInputWebAssets
  SCVBOutputWebAssets
  SCVBMonitorWebAssets
)

target_include_directories(scvb_plugin_common_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src/plugin-common
  ${CMAKE_SOURCE_DIR}/src/core
)

target_compile_definitions(scvb_plugin_common_tests PRIVATE
  JUCE_WEB_BROWSER=1
  # test_web_assets_embedded 从 web/<role>/index.html 现算 import 闭包(不维护手写清单)。
  SCVB_WEB_ROOT="${CMAKE_SOURCE_DIR}/web"
)

if(MSVC)
  target_compile_options(scvb_plugin_common_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_plugin_common_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_plugin_common_tests COMMAND scvb_plugin_common_tests)
# [SL-320] 上界与理由见上面 `scvb_tests` 那段(四套同一份口径,不在这里抄一遍)。
set_tests_properties(scvb_plugin_common_tests PROPERTIES TIMEOUT 30)

# scvb_input_bridge_tests:T30 Input 桥 L0 单测(claim 六值/载荷形状/拒绝语义;链接 JUCE,离线 Catch2)。
# 只编可离线单测的 TU:InputBridgeLogic + BridgeBase(InputEditor 依赖 WebViewHost 真 WebView2
# 实例化,留待 gate 8 真机 GUI pluginval 验证,不编入本测试)。
add_executable(scvb_input_bridge_tests
  webview/test_input_bridge.cpp
  ${CMAKE_SOURCE_DIR}/src/input/InputBridgeLogic.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/BridgeBase.cpp
)

target_link_libraries(scvb_input_bridge_tests PRIVATE
  Catch2::Catch2WithMain
  juce::juce_core
)

target_include_directories(scvb_input_bridge_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src/input
  ${CMAKE_SOURCE_DIR}/src/plugin-common
  ${CMAKE_SOURCE_DIR}/src/core
)

if(MSVC)
  target_compile_options(scvb_input_bridge_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_input_bridge_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_input_bridge_tests COMMAND scvb_input_bridge_tests)
# [SL-320] 上界与理由见上面 `scvb_tests` 那段(四套同一份口径,不在这里抄一遍)。
set_tests_properties(scvb_input_bridge_tests PROPERTIES TIMEOUT 30)

# scvb_ipc_tests:L1 双进程 IPC 契约测试骨架(仅 WIN32;实现由 T07b 填充)。
if(SCVB_BUILD_IPC_TESTS AND WIN32)
  add_subdirectory(ipc)
endif()

# ---------------------------------------------------------------------------
# scvb_host_tests:免 DAW 宿主 harness(T37 三轮验收升级)。
# 一个进程里同时托管 Output 与 Input 两个 Processor + 一个 FakePlayHead,经**真实**共享内存段
# (registry / audio 环 / feat 段 / ctrl 段)通信,手工推 processBlock 并跑 JUCE 消息循环让
# 25Hz Timer 真实触发。四族数据面的回归从此有机器守门,不再靠真机人肉发现。
#
# 只编 *Processor.cpp 与其依赖,**不编** *PluginEntry.cpp:
#   · 那两个 TU 里的 createEditor() 会把 WebViewHost/WebView2 拖进来(harness 不需要 GUI);
#   · createPluginFilter() 在两侧同名,同进程托管两个插件会撞符号。
# 见 src/output/OutputPluginEntry.cpp 头注。
# 守卫与紧邻的 scvb_ipc_tests 对齐:本目标跑**真实命名共享内存段**(Win32 CreateFileMapping),
# 属重量级进程外资源测试,不该在非 Windows 上进默认 ctest 集合。
if(WIN32)
add_executable(scvb_host_tests
  host/test_host_harness.cpp
  ${CMAKE_SOURCE_DIR}/src/output/OutputProcessor.cpp
  ${CMAKE_SOURCE_DIR}/src/output/OutputParams.cpp
  ${CMAKE_SOURCE_DIR}/src/output/OutputAuthority.cpp
  ${CMAKE_SOURCE_DIR}/src/output/AutomationPrinter.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/UiDefaultsStore.cpp
  ${CMAKE_SOURCE_DIR}/src/input/InputProcessor.cpp
  ${CMAKE_SOURCE_DIR}/src/input/InputBridgeLogic.cpp
  ${CMAKE_SOURCE_DIR}/src/plugin-common/BridgeBase.cpp
)

target_link_libraries(scvb_host_tests PRIVATE
  scvb_core
  Catch2::Catch2WithMain
  juce::juce_audio_processors
)

target_include_directories(scvb_host_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src/core
  ${CMAKE_SOURCE_DIR}/src/output
  ${CMAKE_SOURCE_DIR}/src/input
  ${CMAKE_SOURCE_DIR}/src/plugin-common
)

# harness 要扮演宿主的消息线程:runDispatchLoopUntil 让两侧的 25Hz Timer 真实触发。
# 该 API 在 JUCE 里由 JUCE_MODAL_LOOPS_PERMITTED 守卫,**只对本测试目标开**,插件产物不受影响。
target_compile_definitions(scvb_host_tests PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)

if(MSVC)
  # [SL-325 / 顺手修,归属 #204] `/wd4819`:只关这一条、**只关这一个 TU**。
  # 落点是下面的 `set_source_files_properties`(文件级),不是 `target_compile_options`(目标级)——
  # 本目标一共编 9 个 TU,除本文件外那 8 个是 `src/` 下的**产线源码**
  # (OutputProcessor / OutputParams / OutputAuthority / AutomationPrinter / UiDefaultsStore /
  # InputProcessor / InputBridgeLogic / BridgeBase)。目标级会把它们的 C4819 一并关掉,
  # 与本段「只关这一个」的说法对不上(复审【建议】)。文件级之后,作用面与这句话逐字一致。
  #
  # 症状:在**中文 Windows(CP936)**上,这个 TU 报两条
  #   `C4819: 该文件包含不能在当前代码页(0)中表示的字符`(行号指向 :174 / :186),
  # 于是 gates gate 5「/W4 零 warning」判红 —— 而 **CI 上零命中**(runner 的代码页不同),
  # 也就是说这条对 CI 隐形、只打本机,而 gates 才是收口判据。`13392a2`(#204)给本文件
  # 加了 80 行之后开始出现;`9f9a527` 那会儿还是绿的。
  #
  # 关掉它之前,按仓里的三条惯例逐条核过,**都不成立**(所以判定它是工具误报,不是真编码问题):
  #   · **文件编码**:整份是合法 UTF-8(逐字节解码通过);
  #   · **`/utf-8` 是否真作用于本 TU**:是 —— 从 MSBuild 的
  #     `scvb_host_tests.tlog/CL.command.1.tlog` 里取该源文件对应的那条命令行,
  #     实测含 `/utf-8`(不是「vcxproj 里出现过几次」那种间接证据),且无
  #     `/source-charset` / `/execution-charset` 覆盖;
  #   · **加 BOM 能否消除**:**不能** —— 用 .NET 写入 BOM、`Format-Hex` 确认前三字节是
  #     `EF BB BF`、删 obj 强制重编、确认 obj 时间戳变了,C4819 **仍是 2 条**;已还原(sha256 复核)。
  # 反向对照:全仓 95 个源文件含「CP936 表示不了的字符」、**没有一个**带 BOM,
  # 56 个 `.cpp` 在窄字符串字面量里写中文 —— 它们全都编译干净。同样的写法只有这一个 TU 报。
  # 它与别的 TU 唯一量得出来的差别是**体量**:实测 7282 行,第二大的测试 TU 是
  # `tests/ipc/test_ipc_contract.cpp` 1584 行,差 4.6 倍。
  # (上一版这里还写过「唯一开 `/bigobj` 的测试目标」——**假的**,复审指出后实测:`/bigobj`
  # 由 JUCE 的模块 INTERFACE 带进来(`JUCEModuleSupport.cmake`),**链 juce:: 的那五个**
  # 测试目标都有;两个 JUCE-free 的没有 —— `scvb_tests` 与 `scvb_ipc_tests`
  # (后者见 `tests/ipc/CMakeLists.txt:12`,只链 `scvb_core` + `Catch2::Catch2WithMain`)。
  # 仓里的 CMakeLists 一个字都没写过它。
  # ⚠ 「只有 scvb_tests 没有」是这句话的**上一版**,又把 ipc 漏了 —— 那是 `scvb_ipc_tests`
  # 在本卡里**第三次**从我写的枚举里掉出去(前两次:gates 的属性表、上界排序)。
  # 三次同一个原因:清单照着**本文件**写,而 ipc 在隔壁。写「N 个目标怎样」这类全称句,
  # 扫 `tests/**`,别扫本文件。
  # 记在这里是因为那半句曾是「这个 TU 特殊」这条推断的支柱之一,现在支柱只剩体量一条。)
  #
  # 所以这里关的是**一条已确认的误报**,不是把真问题盖住:文件真是 UTF-8、开关真在命令行上。
  # 根治(找到 MSVC 为何在本 TU 上无视 `/utf-8`,或拆分该文件)记在 **SL-325**;
  # 那张卡落地后**请把下面那行删掉**,并用删除式确认删掉后本机 gate 5 仍绿。
  # 给 SL-325 留的线索:报出来的 `:174` / `:186` 两行**本身是纯 ASCII**(是两处 `fprintf`
  # 实参那一行),而该区段的中文全部在 CP936 表示得出来 —— 行号与「哪个字符不可表示」对不上,
  # 所以别照着行号去找「那个字符」。
  target_compile_options(scvb_host_tests PRIVATE /W4 /utf-8 /EHsc)
  set_source_files_properties(host/test_host_harness.cpp PROPERTIES COMPILE_OPTIONS "/wd4819")
else()
  target_compile_options(scvb_host_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_host_tests COMMAND scvb_host_tests)
# [SL-311] 显式上界。此前这一套**没有** TIMEOUT 属性,吃 ctest 的默认 1500s ——
# 一次真挂死要占住机器 25 分钟以上(实测见过 `***Timeout 2278.13 sec`),而它此刻
# 正持着 `Local\SCVB-ipc-tests`,全机别的 agent 一起等。
#
# **数字是量出来的,不是套倍数套出来的**。观测:
#     本机连跑三轮(均 PASS):372.1s / 371.4s / 371.1s
#     **CI(GitHub windows runner,本 PR 的 build-and-validate)**:**223.56s PASS**
#     本机其余单次样本:221.1s(PASS)、223.6s(FAIL)、309.7s(FAIL)、375.9s(FAIL)。
# 注意两件事:①**CI 比开发机快**(223.56s vs 372s)——「runner 更慢所以上界要更宽」
# 这个常见假设在这一套上不成立,900 在 CI 上是 4 倍余量;②本机那三轮 372s 是在
# 同机有别的 agent 在跑时量的,221–224s 才是干净基线。上界按**最慢的观测**留余量,
# 不按最快的。
# 失败的跑**比通过的跑慢**:`HOST L-6b` 是「等报警不来」的形态,失败路径要等满内部截止。
#
# 所以上界必须**同时**满足两条:
#   · 高过最慢的正常观测且留出负载余量 —— 台账记过这一套在负载下从 378s 拖到「跑不完」,
#     上界压到那个数附近会把**正常通过**的跑判成 Timeout,gate 6 变成常红;
#   · 远低于 1500s,让真挂死早点被拦下。
# 取 900s。
#
# ⚠ [SL-320] **口径先后**,写清楚免得下一个人照着一个已经作废的前提去调这个数:
#   · **裁定 900 的当时**,依据是「372s = 稳态」——2.4 倍余量是**照那个前提**算出来的;
#   · **事后才知道**那三轮 372s 是同机有别的 agent 在跑时量的,**含负载**;
#     干净基线是 221–224s(本机 221.1s、CI 223.56s / 223.63s / 223.41s)。
#   · 所以按今天的干净基线,900 的真实余量是 **4 倍**,不是 2.4 倍。
# **数字不改**:900 仍然同时满足上面两条,而且余量比当初以为的更宽,不是更窄;
# 改的只是「它是怎么来的」这句话 —— 留着旧口径,下一个人会拿 372 当基线去推别的结论。
# (这也是 SL-314 那条判例的来处:同机并发会把观测值整体推高,量之前先确认没有别人在跑。)
#
# ⚠ 拦挂死不等于 police 慢:900 只是「明显不对劲」的线,不是性能预算。这一套变慢了
# 该由别的手段发现,不是靠把这个数字调紧 —— 调紧的代价是把 `HOST L-6b` 那类**慢的失败**
# 改判成 Timeout,而那正是 SL-314 在追的线索,改判等于把线索删掉。
#
# ⚠ 这个数字**不负责收残留**。受控复现里(注入永不返回的用例 + TIMEOUT 30)ctest 到点
# 确实杀掉了被测进程,返回后立刻数与 5 秒后再数都是 0;但另一次真实场景里残留确实出现过,
# 成因还没查清。兜底在 `scripts/gates.ps1` gate 6 前后那两次孤儿扫描,不在这里。
set_tests_properties(scvb_host_tests PROPERTIES TIMEOUT 900)
endif()
