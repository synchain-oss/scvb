# scvb_core:纯 C++17 静态库(DSP/分析/IPC/状态/版本引擎)。
# ADR-001/011:不链接 JUCE、不引用 AudioProcessor,可离线 Catch2 单测。
add_library(scvb_core STATIC
  ScvbCore.cpp
  ScvbCore.h
)

target_include_directories(scvb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
  target_compile_options(scvb_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(scvb_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
