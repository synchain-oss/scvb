// SPDX-License-Identifier: GPL-3.0-or-later
// 本文件由 scripts/gen-design-box.py 从 web/shared/design-box.js 自动生成 —— 请勿手工编辑。
// 设计盒常量唯一真源 = web/shared/design-box.js(05 §1.2;07 T27 设计定稿回流①)。
// 改值请编辑 design-box.js 后重跑:python scripts/gen-design-box.py
#pragma once

#include <array>

namespace scvb::design
{
// Output 设计盒(1x,05 §1.2):1180×780,7 档缩放。
inline constexpr int kOutputDesignW = 1180;
inline constexpr int kOutputDesignH = 780;
inline constexpr std::array<double, 7> kOutputPresets = {0.5, 0.65, 0.8, 1, 1.25, 1.5, 2};

// Input 设计盒(1x,05 §1.2):460×560,10 档缩放。
inline constexpr int kInputDesignW = 460;
inline constexpr int kInputDesignH = 560;
inline constexpr std::array<double, 10> kInputPresets = {0.33, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 2.5, 3};

// 缩放档位计数(与 design-box.js 数组长度一致)。
inline constexpr int kOutputPresetCount = 7;
inline constexpr int kInputPresetCount = 10;
} // namespace scvb::design
