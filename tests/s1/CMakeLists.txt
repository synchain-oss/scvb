# scvb_s1_tests:S1 路由 spike 离线验证(10-validation §1.1 能离线判定的部分)。
# 链接 spikes/s1 的 s1_core(纯 C++17 + Windows SDK)+ Catch2,不链 JUCE。
add_executable(scvb_s1_tests
  test_s1_routing.cpp
)

target_link_libraries(scvb_s1_tests PRIVATE s1_core Catch2::Catch2WithMain)
target_include_directories(scvb_s1_tests PRIVATE ${CMAKE_SOURCE_DIR}/spikes/s1)

if(MSVC)
  target_compile_options(scvb_s1_tests PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(scvb_s1_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME scvb_s1_tests COMMAND scvb_s1_tests)
