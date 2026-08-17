# scvb_ipc_tests:L1 双进程 IPC 契约测试(Catch2 + Windows API,仅 WIN32;10-validation §2.3)。
# 父进程经 CreateProcessW 拉起 tests/tools/scvb_ipc_peer(对端进程),共享 tests/ipc/ipc_contract_harness.h
# 的环读写语义。IPC-1..20 共 26 条验收真源 = masterPlan 10-validation §2.3。
add_executable(scvb_ipc_tests
  test_ipc_contract.cpp
)

target_link_libraries(scvb_ipc_tests PRIVATE scvb_core Catch2::Catch2WithMain)
target_include_directories(scvb_ipc_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
# 对端进程路径(供测试 CreateProcessW 拉起;TARGET_FILE 在 generate 期求值,跨目录引用 OK)。
target_compile_definitions(scvb_ipc_tests PRIVATE SCVB_IPC_PEER_PATH=L"$<TARGET_FILE:scvb_ipc_peer>")

if(MSVC)
  target_compile_options(scvb_ipc_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_ipc_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_ipc_tests COMMAND scvb_ipc_tests)
set_tests_properties(scvb_ipc_tests PROPERTIES TIMEOUT 900)
