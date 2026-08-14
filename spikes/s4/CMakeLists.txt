# S4 状态体量 spike(J16:验证代码,不进生产路径,由 T19 的 PR 删除)。
# s4_state:spike 自有 state 编解码 + 满配夹具;链接 juce_core(juce::GZIPCompressorOutputStream)
#   与 juce_cryptography(juce::SHA256)——与生产侧 ADR-011 的压缩/校验栈一致。
# scvb_s4_state:控制台验收跑分(计时方法学复用 T04 scvb_bench)。

add_library(s4_state STATIC
  StateFixture.cpp
  StateCodec.cpp
  StateFixture.h
  StateCodec.h
  StateSchema.h
  ByteIo.h
)

target_include_directories(s4_state PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(s4_state PUBLIC juce::juce_core juce::juce_cryptography)

if(MSVC)
  target_compile_options(s4_state PRIVATE /W4 /utf-8)
else()
  target_compile_options(s4_state PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(scvb_s4_state
  S4StateBench.cpp
)

target_link_libraries(scvb_s4_state PRIVATE s4_state)

if(MSVC)
  target_compile_options(scvb_s4_state PRIVATE /W4 /utf-8)
else()
  target_compile_options(scvb_s4_state PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 快速回环单测:小夹具跑 5 项验收机制(不断言耗时/体量阈值),进 ctest(gate 6)。
add_test(NAME scvb_s4_state COMMAND scvb_s4_state --quick)
