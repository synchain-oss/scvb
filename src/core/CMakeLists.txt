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
  analysis/Reanalysis.cpp
  analysis/Reanalysis.h
  analysis/Hungarian.cpp
  analysis/Hungarian.h
  analysis/AnalysisPipeline.cpp
  analysis/AutoAssign.cpp
  analysis/AutoAssign.h
  ipc/SegmentLayout.h
  ipc/ISegmentBackend.h
  ipc/SegmentBackendWin32.h
  ipc/GroupProbe.h
  ipc/GroupProbe.cpp
  ipc/Registry.cpp
  ipc/Registry.h
  ipc/RegistryProbe.h
  ipc/CtrlPlane.cpp
  ipc/CtrlPlane.h
  ipc/FeatRing.cpp
  ipc/FeatRing.h
  ipc/SegmentBackendInProcess.cpp
  ipc/SegmentBackendInProcess.h
  ipc/AudioRing.cpp
  ipc/AudioRing.h
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
  state/StateCodec.cpp
  state/StateCodec.h
  state/StateMigration.cpp
  state/StateMigration.h
  state/FeaturesCodec.cpp
  state/FeaturesCodec.h
  state/SidecarStore.cpp
  state/SidecarStore.h
  state/InputStateCodec.cpp
  state/InputStateCodec.h
  state/OutputStateCodec.cpp
  state/OutputStateCodec.h
  state/SegmentEdit.cpp
  state/SegmentEdit.h
)

# SegmentBackendWin32.cpp 仅 Windows 命名共享内存(CreateFileMappingW);Posix 实现归 v2。
if(WIN32)
  target_sources(scvb_core PRIVATE ipc/SegmentBackendWin32.cpp ipc/RegistryProbe.cpp)
endif()

target_include_directories(scvb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

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