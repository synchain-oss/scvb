# scvb_ipc_tests:L1 双进程 IPC 契约测试(Catch2 + Windows API,仅 WIN32;10-validation §2.3)。
# 父进程经 CreateProcessW 拉起 tests/tools/scvb_ipc_peer(对端进程),共享 tests/ipc/ipc_contract_harness.h
# 的环读写语义。IPC-1..20 共 26 条验收真源 = masterPlan 10-validation §2.3。
add_executable(scvb_ipc_tests
  test_ipc_contract.cpp
)

target_link_libraries(scvb_ipc_tests PRIVATE scvb_core Catch2::Catch2WithMain)
target_include_directories(scvb_ipc_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
# 对端进程路径不再编译期烘焙 $<TARGET_FILE>(VS 生成器渲染反斜杠路径会触发 MSVC C4129 或
# 损坏转义,PR#46 复审);改由 test_ipc_contract.cpp 运行期 GetModuleFileNameW 推导(Ninja 单配置
# 与 VS 多配置两态都探测),见其 peerExe()。

if(MSVC)
  target_compile_options(scvb_ipc_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_ipc_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_ipc_tests COMMAND scvb_ipc_tests)
set_tests_properties(scvb_ipc_tests PROPERTIES TIMEOUT 900)
