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
