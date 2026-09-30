// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// scvb_core:SCVB 共享核心库(ADR-001)。
// 纯 C++17(STL + Windows SDK),不链接 JUCE —— 保证离线单测秒级编译跑测。

namespace scvb
{
// 返回核心库版本串(T01 冒烟用例用它证明 scvb_core 可链接、可调用)。
// 版本串单一真源 = 顶层 CMakeLists 的 project(SCVB VERSION ...):src/core/CMakeLists.txt 把它作为
// 编译定义 SCVB_VERSION_STRING 注入 ScvbCore.cpp(那里的 kScvbCoreVersion),本头文件不写版本字面量。
const char* coreVersion();
} // namespace scvb
