#pragma once

// scvb_core:SCVB 共享核心库(ADR-001)。
// 纯 C++17(STL + Windows SDK),不链接 JUCE —— 保证离线单测秒级编译跑测。

namespace scvb
{
// 版本串单一真源 = 顶层 CMakeLists 的 project(SCVB VERSION ...)。
// 此处仅为测试链接冒烟暴露一个可调用的符号。
inline constexpr const char* kScvbCoreVersion = "0.1.0";

// 返回核心库版本串(T01 冒烟用例用它证明 scvb_core 可链接、可调用)。
const char* coreVersion();
} // namespace scvb
