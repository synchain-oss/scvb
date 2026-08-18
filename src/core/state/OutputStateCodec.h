// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OutputStateCodec —— Output 插件 state 的紧凑二进制编解码(params-v0 §二 的最小 T24 子集)。
// JUCE-free,可离线单测。外层经 T19 StateCodec 的 TLV 容器承载(chunk fourcc = CFGS);
// payload 对容器而言是不透明字节(与 Input 的 CFGS 同为紧凑二进制,互不相扰)。
//
// T24 范围字段:group_id([J66],默认 1)、capture_enabled、output_enabled、version_active、ui。
// 完整 Output state(analysis/channels[15]/versions[2]/features)由后续任务扩展本 codec。
//
// payload 布局(little-endian):
//   0  u32 groupId(1..8)
//   4  u32 captureEnabled(0|1)
//   8  u32 outputEnabled(0|1)
//   12 u32 versionActive(1..2)
//   16 u32 uiScale(percent)
//   20 u32 languageBytes
//   24.. languageBytes 个 UTF-8 字节
//
// decode 处理不可信字节:长度/范围字段先校验再用于分配或索引(CLAUDE.md §7.3)。

#include <cstdint>
#include <string>
#include <vector>

namespace scvb::state
{

inline constexpr std::uint32_t kOutputGroupIdMin = 1; // [J66] 1..8
inline constexpr std::uint32_t kOutputGroupIdMax = 8;
inline constexpr std::uint32_t kOutputDefaultGroupId = 1; // [J66] 默认 1(UI 显示 A)
inline constexpr std::uint32_t kOutputVersionMin = 1; // [J59] 1..2
inline constexpr std::uint32_t kOutputVersionMax = 2;
inline constexpr std::uint32_t kOutputLanguageMaxBytes = 64;

struct OutputState
{
    std::uint32_t groupId = kOutputDefaultGroupId; // 1..8
    std::uint32_t captureEnabled = 0; // 采集开关(默认 off)
    std::uint32_t outputEnabled = 1; // 输出开关(默认 on = 引擎权威)
    std::uint32_t versionActive = 1; // 活动版本(1..2)
    std::uint32_t uiScale = 100; // percent
    std::string uiLanguage = "en";
};

// 编码;语言超长截断(≤kOutputLanguageMaxBytes)。返回 false = 无法分配。
bool encodeOutputState(const OutputState& s, std::vector<std::uint8_t>& out);

// 解码;严格校验(长度/范围)。失败 → 返回 false 且 out 保持默认(不半填充)。
bool decodeOutputState(const std::uint8_t* data, std::size_t size, OutputState& out);

} // namespace scvb::state
