# scvb_ipc_tests:L1 双进程 IPC 契约测试(Catch2;10-validation §2.3)。[B 线 M12a] 起 Windows 与 macOS 都编
# (上一级 tests/CMakeLists.txt 的条件)。父进程经 tests/support/peer_spawn.h 拉起 tests/tools/scvb_ipc_peer
# (对端进程;Windows CreateProcessW / POSIX posix_spawn),共享 tests/ipc/ipc_contract_harness.h 的环读写语义。
# IPC-1..20 共 26 条验收真源 = masterPlan 10-validation §2.3。
add_executable(scvb_ipc_tests
  test_ipc_contract.cpp
  test_registry_probe.cpp
  # [T44/J75] viz 段 L1:**独立文件**,不并进 test_ipc_contract.cpp —— 那份正被 IPC-16 flake
  # 修复一路系统性改动,合并冲突面必须归零(spawn helper 走 tests/support/peer_spawn.h)。
  test_ipc_viz.cpp
)

# [B 线 M12a] 只在 macOS 上编的格([ipc][posix]):POSIX 后端特有的跨进程生命周期(最后一个离开者
# shm_unlink、崩溃留下的无主残段由下一个创建者清掉重建),以及 support/exclusive_guard.h 的 POSIX 守卫
# 真的拒绝同机第二个测试进程。Windows 上段由内核按句柄引用计数回收,没有「残段」这一态,这几格无对应物;
# 独立追加块,上面那份源列表与 Windows 上的用例集合逐字不变。
if(APPLE)
  target_sources(scvb_ipc_tests PRIVATE test_ipc_posix.cpp)
endif()

target_link_libraries(scvb_ipc_tests PRIVATE scvb_core Catch2::Catch2WithMain)
target_include_directories(scvb_ipc_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/tests)
# 对端进程路径不再编译期烘焙 $<TARGET_FILE>(VS 生成器渲染反斜杠路径会触发 MSVC C4129 或
# 损坏转义,PR#46 复审);改由 tests/support/peer_spawn.h 运行期从测试 exe 自身路径推导(Ninja 单配置
# 与 VS 多配置两态都探测),见其 peerExe()。构建依赖挂在 tests/tools/CMakeLists.txt(那时对端目标才存在)。

if(MSVC)
  target_compile_options(scvb_ipc_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_ipc_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_ipc_tests COMMAND scvb_ipc_tests)
set_tests_properties(scvb_ipc_tests PROPERTIES TIMEOUT 900)
