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
  analysis/AutoAssign.cpp
  analysis/AutoAssign.h
  ipc/SegmentLayout.h
  ipc/ISegmentBackend.h
  ipc/SegmentBackendWin32.h
  ipc/Registry.cpp
  ipc/Registry.h
  ipc/CtrlPlane.cpp
  ipc/CtrlPlane.h
  ipc/FeatRing.cpp
  ipc/FeatRing.h
  ipc/SegmentBackendInProcess.cpp
  ipc/SegmentBackendInProcess.h
)

# SegmentBackendWin32.cpp 仅 Windows 命名共享内存(CreateFileMappingW);Posix 实现归 v2。
if(WIN32)
  target_sources(scvb_core PRIVATE ipc/SegmentBackendWin32.cpp)
endif()

target_include_directories(scvb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
  target_compile_options(scvb_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(scvb_core PRIVATE -Wall -Wextra -Wpedantic)
endif()