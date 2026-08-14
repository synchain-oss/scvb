# 验证工具箱(10-validation §2.1 / §4.2):scvb_nulltest 控制台 wav 比对器 + click 素材生成器。
# 任务 T01c。零外部依赖,不链接 JUCE / Catch2(ADR-001:工具可离线在任何机器运行)。
add_executable(scvb_nulltest
  scvb_nulltest.cpp
)

if(MSVC)
  target_compile_options(scvb_nulltest PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_nulltest PRIVATE -Wall -Wextra -Wpedantic)
endif()

# scvb_diag:S1 诊断导出(J09 正式通道,常驻仓库)。读 ctrl/registry/audio 环头,按 1Hz 追加 csv。
# 仅 WIN32(Windows 命名共享内存);零外部依赖(只含 spikes/s1 的 SegmentLayout.h 头)。
if(WIN32)
  add_executable(scvb_diag
    scvb_diag.cpp
  )

  target_include_directories(scvb_diag PRIVATE ${CMAKE_SOURCE_DIR}/spikes/s1)
  target_compile_definitions(scvb_diag PRIVATE _CRT_SECURE_NO_WARNINGS)

  if(MSVC)
    target_compile_options(scvb_diag PRIVATE /W4 /utf-8 /EHsc)
  else()
    target_compile_options(scvb_diag PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()
