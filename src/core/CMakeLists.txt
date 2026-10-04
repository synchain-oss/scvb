# scvb_core:纯 C++17 静态库(DSP/分析/IPC/状态/版本引擎)。
# ADR-001/011:不链接 JUCE、不引用 AudioProcessor,可离线 Catch2 单测。
add_library(scvb_core STATIC
  ScvbCore.cpp
  ScvbCore.h
  dsp/KWeighting.cpp
  dsp/KWeighting.h
  dsp/ParamSmoother.h
  analysis/FeatureExtractor.cpp
  analysis/FeatureExtractor.h
  analysis/CoverageMap.cpp
  analysis/CoverageMap.h
  analysis/FrameStore.cpp
  analysis/FrameStore.h
  analysis/FeatureFingerprint.cpp
  analysis/FeatureFingerprint.h
  analysis/PanCurve.cpp
  analysis/PanCurve.h
  dsp/PanMath.h
  engine/CurveEvaluator.cpp
  engine/CurveEvaluator.h
  engine/AuthorityMode.cpp
  engine/AuthorityMode.h
  engine/DspArbiter.cpp
  engine/DspArbiter.h
  engine/PlayheadShot.h
  engine/VersionStore.cpp
  engine/VersionStore.h
  analysis/Loudness.cpp
  analysis/Loudness.h
  analysis/EnergyVad.cpp
  analysis/EnergyVad.h
  analysis/IVadBackend.h
  analysis/LoudnessMode.cpp
  analysis/LoudnessMode.h
  analysis/Segmentation.cpp
  analysis/Segmentation.h
  analysis/WaveValleys.cpp
  analysis/WaveValleys.h
  analysis/Reanalysis.cpp
  analysis/Reanalysis.h
  analysis/Hungarian.cpp
  analysis/Hungarian.h
  analysis/AnalysisPipeline.cpp
  analysis/VadPreview.cpp
  analysis/VadPreview.h
  analysis/AutoAssign.cpp
  analysis/AutoAssign.h
  analysis/LeadTimeline.cpp
  analysis/LeadTimeline.h
  ipc/SegmentLayout.h
  ipc/ISegmentBackend.h
  ipc/SegmentBackendWin32.h
  ipc/PlatformSegmentBackend.h
  ipc/GroupProbe.h
  ipc/GroupProbe.cpp
  ipc/Registry.cpp
  ipc/Registry.h
  ipc/CtrlPlane.cpp
  ipc/CtrlPlane.h
  ipc/FeatRing.cpp
  ipc/FeatRing.h
  ipc/SegmentBackendInProcess.cpp
  ipc/SegmentBackendInProcess.h
  ipc/AudioRing.cpp
  ipc/AudioRing.h
  ipc/VizPlane.cpp
  ipc/VizPlane.h
  input/InputSession.cpp
  input/InputSession.h
  input/OutputStage.cpp
  input/OutputStage.h
  output/IMixSource.h
  output/ShmRingMixSource.cpp
  output/ShmRingMixSource.h
  output/MixMath.h
  output/BusXfade.cpp
  output/BusXfade.h
  output/OutputSession.cpp
  output/OutputSession.h
  output/DistReadback.h
  output/VizPublisher.cpp
  output/VizPublisher.h
  state/StateCodec.cpp
  state/StateCodec.h
  state/StateMigration.cpp
  state/StateMigration.h
  state/FeaturesCodec.cpp
  state/FeaturesCodec.h
  state/FeaturesSnapshot.cpp
  state/FeaturesSnapshot.h
  state/SidecarStore.cpp
  state/SidecarStore.h
  state/InputStateCodec.cpp
  state/InputStateCodec.h
  state/OutputStateCodec.cpp
  state/OutputStateCodec.h
  state/SegmentEdit.cpp
  state/SegmentEdit.h
  export/SuggestionExport.cpp
  export/SuggestionExport.h
)

# SegmentBackendWin32.cpp 仅 Windows 命名共享内存(CreateFileMappingW);Posix 实现由 B 线 M05 落地。
# 插件侧按平台选哪个后端,见 ipc/PlatformSegmentBackend.h。跨组只读探测统一走 ipc/GroupProbe.cpp
# (经后端抽象,全平台编译);原先只在这里编的 Win32 专用 RegistryProbe 已于 B 线 M03 退役。
if(WIN32)
  target_sources(scvb_core PRIVATE ipc/SegmentBackendWin32.cpp)
endif()

target_include_directories(scvb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 核心库版本串(scvb::coreVersion())取自顶层 project(SCVB VERSION ...),源码里不写字面量。
# 用 SCVB_VERSION(由 project(SCVB ...) 设定)而不是 PROJECT_VERSION:后者指「最近一次 project()
# 调用」,名字上不绑定本项目。
# PRIVATE:只有 ScvbCore.cpp 读它,不把宏带进链接 scvb_core 的其他目标。
# 对拍在 tests/core/test_smoke.cpp:它直接读顶层 CMakeLists.txt 的那一行,不经这个定义。
target_compile_definitions(scvb_core PRIVATE SCVB_VERSION_STRING="${SCVB_VERSION}")

# T21:miniz(vendored,third_party/miniz;zlib 兼容,gzip 用,ADR-011 零新依赖)。
# 第三方源码按 C++ 编译并关警告(不污染 /W4 零 warning 门禁);miniz.h 仅由 FeaturesCodec.cpp 内部包含。
target_include_directories(scvb_core PRIVATE ${CMAKE_SOURCE_DIR}/third_party/miniz)
target_sources(scvb_core PRIVATE ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c)
set_source_files_properties(${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c PROPERTIES LANGUAGE CXX)
if(MSVC)
  set_source_files_properties(${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "/w")
else()
  set_source_files_properties(${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
endif()

if(MSVC)
  target_compile_options(scvb_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(scvb_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# [B 线 M06a] 非 MSVC 编译器关掉浮点乘加融合(-ffp-contract=off)。clang 默认 `on`:同一条语句里的
# a*b+c 会被融成一条 fmadd(arm64 上必然发生),中间积不再舍入。于是「同一个表达式算两遍、相减得 0」
# 这类恒等式不再成立 —— 实测 BAL-3(tests/core/test_balance.cpp)在 arm64 上因此红:
# AutoAssign.cpp 的 `delta = lTarget - 10*log10(z)`,在该用例里 lTarget 与 10*log10(z) 是同一个量,
# 融合成 fma 之后差却成了一个打印为 -0.0 的极小负数,`u == 0.0` 断言落空。
# MSVC 默认不融合(/fp:precise 下要显式 /fp:contract 才融),这一条让两个平台的求值语义对齐。
# PUBLIC:scvb_core 头文件里的内联数学(PanMath / MixMath 等)会在链接方自己的编译单元里展开,
# 链接方(测试、插件)必须同口径,否则同一个内联函数在两个编译单元里能算出不同的末位。
if(NOT MSVC)
  target_compile_options(scvb_core PUBLIC -ffp-contract=off)
endif()
