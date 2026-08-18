// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// InputStateCodec —— Input 插件 state 的紧凑二进制编解码(params-v0 §三)。JUCE-free,可离线单测。
//
// Input state = abi(容器字段,归 StateCodec)+ channel_id + group_id + ui{scale, language}。
// 本 codec 只负责 Input 特有字段(channel_id / group_id / ui)的 payload 编解码,外层仍经
// T19 StateCodec 的 TLV 容器承载(chunk fourcc 复用 CFGS,payload 对本 codec 而言是紧凑二进制,
// 对容器而言是不透明字节 —— 与 Output 的 ValueTree CFGS 同为不透明 payload,互不相扰)。
//
// payload 布局(little-endian):
//   0  u32 channelId(0..15,0=未分配)
//   4  u32 groupId(1..8)
//   8  u32 uiScale(percent)
//   12 u32 languageBytes
//   16.. languageBytes 个 UTF-8 字节
//
// decode 处理不可信字节:长度/范围字段先校验再用于分配或索引(CLAUDE.md §7.3)。

#include <cstdint>
#include <string>
#include <vector>

namespace scvb::state
{

inline constexpr std::uint32_t kInputChannelIdMax = 15; // [J01+J59] 0..15,0=未分配
inline constexpr std::uint32_t kInputGroupIdMin = 1; // [J66] 1..8
inline constexpr std::uint32_t kInputGroupIdMax = 8;
inline constexpr std::uint32_t kInputDefaultGroupId = 1; // [J66] 默认 1(UI 显示 A)
inline constexpr std::uint32_t kInputLanguageMaxBytes = 64;

struct InputState
{
    std::uint32_t channelId = 0; // 0 = 未分配(首次插入默认)
    std::uint32_t groupId = kInputDefaultGroupId; // 1..8
    std::uint32_t uiScale = 100; // percent
    std::string uiLanguage = "en";
};

// 编码;语言超长截断(≤kInputLanguageMaxBytes)。返回 false = 无法分配。
bool encodeInputState(const InputState& s, std::vector<std::uint8_t>& out);

// 解码;严格校验(长度/范围)。失败 → 返回 false 且 out 保持默认(不半填充)。
bool decodeInputState(const std::uint8_t* data, std::size_t size, InputState& out);

} // namespace scvb::state
