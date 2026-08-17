# S1 路由可靠性 spike(J16:验证代码,不进生产路径 src/core/ipc/,由 T06 的 PR 在正式版落地后删除)。
# s1_core(纯 C++17 + Windows SDK,不链 JUCE,可离线 Catch2 单测)+ 两个 VST3 插件。
# 段布局/后端/Registry/环的命名与生产版保持同名,正式任务 = 移植 + 加固而非重写。

add_library(s1_core STATIC
  SegmentBackendWin32.cpp
  Registry.cpp
  AudioRing.cpp
  ScvbJournal.cpp
  SegmentBackendWin32.h
  Registry.h
  AudioRing.h
  ScvbJournal.h
  SegmentLayout.h
  OutputStage.h
  BusXfade.h
)

target_include_directories(s1_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# getenv/strtol 触发 MSVC C4996(CRT 安全函数建议);本项目 C++17 标准库用法,显式关闭。
target_compile_definitions(s1_core PUBLIC _CRT_SECURE_NO_WARNINGS)

if(MSVC)
  target_compile_options(s1_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(s1_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# SCVBInput:Input 插件(VST3)。ADR-001:PLUGIN_CODE=Scvi,厂商码 Snch。
# ---------------------------------------------------------------------------
juce_add_plugin(SCVBInput
  COMPANY_NAME             "Synchain"
  PLUGIN_MANUFACTURER_CODE Snch
  PLUGIN_CODE              Scvi
  FORMATS                  VST3
  PRODUCT_NAME             "SCVB Input"

  # "Fx" 由 JUCE 的 VST3 wrapper 自动添加
  VST3_CATEGORIES          "Fx"

  BUNDLE_ID                "com.synchain.scvb.input"
)

target_sources(SCVBInput PRIVATE
  InputProcessor.h
  InputProcessor.cpp
)

target_link_libraries(SCVBInput PRIVATE s1_core)

if(MSVC)
  target_compile_options(SCVBInput PRIVATE /W4 /utf-8)
else()
  target_compile_options(SCVBInput PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(SCVBInput PUBLIC
  JUCE_VST3_CAN_REPLACE_VST2=0
)

# ---------------------------------------------------------------------------
# SCVBOutput:Output 插件(VST3)。ADR-001:PLUGIN_CODE=Scvo,厂商码 Snch。
# ---------------------------------------------------------------------------
juce_add_plugin(SCVBOutput
  COMPANY_NAME             "Synchain"
  PLUGIN_MANUFACTURER_CODE Snch
  PLUGIN_CODE              Scvo
  FORMATS                  VST3
  PRODUCT_NAME             "SCVB Output"

  VST3_CATEGORIES          "Fx"

  BUNDLE_ID                "com.synchain.scvb.output"
)

target_sources(SCVBOutput PRIVATE
  OutputProcessor.h
  OutputProcessor.cpp
)

target_link_libraries(SCVBOutput PRIVATE s1_core)

if(MSVC)
  target_compile_options(SCVBOutput PRIVATE /W4 /utf-8)
else()
  target_compile_options(SCVBOutput PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(SCVBOutput PUBLIC
  JUCE_VST3_CAN_REPLACE_VST2=0
)

# ---------------------------------------------------------------------------
# scvb_stress:S1 免 DAW 压测宿主(v6)。链接 s1_core,命令行复现宿主编排
# (freeze/RIP/导出的 mono⇄stereo 重建)。每场景独立进程运行,崩溃由 WER 转储。
# ---------------------------------------------------------------------------
add_executable(scvb_stress
  scvb_stress.cpp
)

target_link_libraries(scvb_stress PRIVATE s1_core)

if(MSVC)
  target_compile_options(scvb_stress PRIVATE /W4 /utf-8)
else()
  target_compile_options(scvb_stress PRIVATE -Wall -Wextra -Wpedantic)
endif()

# CTest 注册:每场景独立进程(崩溃 = 测试失败,进程异常退出码直接判红)。
# 环帧数压到 1<<13 让「越界位置」几块之内到达(mono2stereo 首个 stereo 块即越界)。
foreach(_scenario mono2stereo flip outputcheck churn multiclaim)
  add_test(NAME s1_stress_${_scenario} COMMAND scvb_stress ${_scenario})
  set_tests_properties(s1_stress_${_scenario} PROPERTIES ENVIRONMENT "SCVB_RING_FRAMES=8192")
endforeach()

