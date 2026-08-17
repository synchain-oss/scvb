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
# 仅 WIN32(Windows 命名共享内存);零外部依赖(只含 src/core/ipc 的 SegmentLayout.h 头)。
if(WIN32)
  add_executable(scvb_diag
    scvb_diag.cpp
  )

  target_include_directories(scvb_diag PRIVATE ${CMAKE_SOURCE_DIR}/src/core/ipc)
  target_compile_definitions(scvb_diag PRIVATE _CRT_SECURE_NO_WARNINGS)

  if(MSVC)
    target_compile_options(scvb_diag PRIVATE /W4 /utf-8 /EHsc)
  else()
    target_compile_options(scvb_diag PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

# scvb_ipc_peer:L1 双进程 IPC 契约测试的对端进程(10-validation §2.1/§2.3,T07b)。
# 仅 WIN32;链接 scvb_core;共享 tests/ipc/ipc_contract_harness.h 的环读写语义。
if(WIN32)
  add_executable(scvb_ipc_peer
    scvb_ipc_peer.cpp
  )

  target_link_libraries(scvb_ipc_peer PRIVATE scvb_core)
  target_include_directories(scvb_ipc_peer PRIVATE ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/tests/ipc)

  if(MSVC)
    target_compile_options(scvb_ipc_peer PRIVATE /W4 /utf-8 /EHsc)
  else()
    target_compile_options(scvb_ipc_peer PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

# scvb_stress:S1 免 DAW 压测宿主(随 T06 自 spikes/s1 迁入,暂不编译)。依赖 AudioRing/Registry
# 生产实现(归 T07/T20),落地后重新接线;场景已由 T07 InProcess + T07b L1 覆盖。
# 本 .cpp 不加入任何 target(其 include 的 AudioRing.h/Registry.h 已随 spike 删除)。

# scvb_bench:性能基准 + 内置最小离线 host(10-validation §5.4;T04 交付、T05 复用)。
# 链接 scvb_core(纯 C++17,零 JUCE),可离线在任何机器运行。--dsp 出 PERF-1..4 判定依据,
# --render 出数学层端到端(不能替代 DAW 验证,见其 --help)。
add_executable(scvb_bench
  scvb_bench.cpp
)

target_link_libraries(scvb_bench PRIVATE scvb_core)
target_include_directories(scvb_bench PRIVATE ${CMAKE_SOURCE_DIR}/src/core)

if(MSVC)
  target_compile_options(scvb_bench PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()