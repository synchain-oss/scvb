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
// [J75] T43:ui.master_chart_mode **不属**本 CFGS payload —— 它由独立 chunk 'UICF'
// (kFourccUiConfig,定长 4 字节 u32)承载,见 encodeUiConfig / decodeUiConfig。旧版本读新工程时,
// 不认识的 UICF 块按容器「未知 fourcc 原样保留回写」机制零破坏保真,不与 CFGS/CRVS 解析纠缠;
// 旧工程无 UICF 块 → 默认 distribution。见变更文档 docs/contract-changes/20260825-master-chart-mode.md。
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
// [J75] T43:ui.master_chart_mode 独立 UICF chunk 载荷(u32,0/1);未知值解码回落 distribution。
inline constexpr std::uint32_t kMasterChartModeDistribution = 0; // 默认档
inline constexpr std::uint32_t kMasterChartModeTrajectory = 1;
inline constexpr std::uint32_t kUiConfigBytes = 4; // UICF payload 定长 4 字节

struct OutputState
{
    std::uint32_t groupId = kOutputDefaultGroupId; // 1..8
    std::uint32_t captureEnabled = 0; // 采集开关(默认 off)
    std::uint32_t outputEnabled = 1; // 输出开关(默认 on = 引擎权威)
    std::uint32_t versionActive = 1; // 活动版本(1..2)
    std::uint32_t uiScale = 100; // percent
    std::string uiLanguage = "en";
    // masterChartMode 不属 CFGS:由独立 UICF chunk(kFourccUiConfig)承载,见 encodeUiConfig/decodeUiConfig。
};

// 编码;语言超长截断(≤kOutputLanguageMaxBytes)。返回 false = 无法分配。
bool encodeOutputState(const OutputState& s, std::vector<std::uint8_t>& out);

// 解码;严格校验(长度/范围)。失败 → 返回 false 且 out 保持默认(不半填充)。
bool decodeOutputState(const std::uint8_t* data, std::size_t size, OutputState& out);

// [J75] T43:ui.master_chart_mode 的独立 UICF chunk 载荷编解码(定长 4 字节 u32)。
// encode:恒写 4 字节(0=distribution | 1=trajectory)。
// decode:长度 != 4 → 返回 false(§7.3 拒载,调用方回落默认);未知取值(≥2)→ 回落默认 distribution。
bool encodeUiConfig(std::uint32_t masterChartMode, std::vector<std::uint8_t>& out);
bool decodeUiConfig(const std::uint8_t* data, std::size_t size, std::uint32_t& out);

} // namespace scvb::state
