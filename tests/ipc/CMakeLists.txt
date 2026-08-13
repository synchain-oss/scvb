# scvb_ipc_tests:L1 双进程 IPC 契约测试骨架(Catch2 + Windows API,仅 WIN32)。
# 实现由 T07b 填充(10-validation §2.3);本卡仅建目标骨架,保证可构建、可被 ctest 发现。
add_executable(scvb_ipc_tests
  test_ipc_contract.cpp
)

target_link_libraries(scvb_ipc_tests PRIVATE scvb_core Catch2::Catch2WithMain)
target_include_directories(scvb_ipc_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/core)

if(MSVC)
  target_compile_options(scvb_ipc_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_ipc_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_ipc_tests COMMAND scvb_ipc_tests)
