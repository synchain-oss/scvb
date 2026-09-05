// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OutputStateCodec —— Output 插件 state 的紧凑二进制编解码(params-v0 §二 的最小 T24 子集)。
// JUCE-free,可离线单测。外层经 T19 StateCodec 的 TLV 容器承载(chunk fourcc = CFGS);
// payload 对容器而言是不透明字节(与 Input 的 CFGS 同为紧凑二进制,互不相扰)。
//
// T24 范围字段:group_id([J66],默认 1)、capture_enabled、output_enabled、version_active、ui。
// 完整 Output state(analysis/channels[15]/versions[2]/features)由后续任务扩展本 codec。
// [J69/U24] analysis.loudness_mode / center_slot_policy 两字段由本 codec 持久化(评审遗留:
// T35 #62 落盘缺口;STATE_SCHEMA 口径默认 kw_integrated / priority_queue)。
//
// payload 布局(little-endian):
//   0  u32 groupId(1..8)
//   4  u32 captureEnabled(0|1)
//   8  u32 outputEnabled(0|1)
//   12 u32 versionActive(1..2)
//   16 u32 uiScale(percent)
//   20 u32 languageBytes
//   24.. languageBytes 个 UTF-8 字节
//   24+languageBytes  u32 loudnessMode(0=kw_integrated,1=rms,2=peak_dbfs)
//   28+languageBytes  u32 centerSlotPolicy(0=priority_queue,1=lead_exclusive,2=even_spread)
//   32+languageBytes  u32 appliedLoudnessMode([SL-279] 上次全量分析所用,同一套序号)
//   36+languageBytes  u32 appliedCenterSlotPolicy([SL-278/SL-279] 同上)
//   40+languageBytes.. 未知尾部(未来小版本追加字段;解码保留、编码原样回写,防静默丢字段)
//
// 兼容:**两级长度回退**,各对应一次 abi 升格 ——
//   · 旧版(abi=1)payload 无「当前」那两个 u32(24+languageBytes 即止)→ 两字段回落默认
//     且不计未知回落(经 migrate_1_to_2 no-op + 本 codec 长度回退);
//   · 旧版(abi=2)payload 有「当前」、无 applied 那两个 u32 → [SL-279] **applied := 当前值**,
//     **不是回落默认**。语义是「这份旧工程视为已经按它存着的那档分析过」——
//     回落默认会让一个存了非默认档的旧工程一打开就报「需重新分析」,那是误报,
//     而 applied := 当前正是今天 UI 的意图。migrate_2_to_3 因此同样是 no-op,不重写 payload。
// ⚠ [SL-279 复审] **两级回退把 `unknownTail` 的前向兼容粒度收窄了**,记在这里免得下一个人
//   踩到自己写的门上:尾部长度只接受 0 / 8 / 16+ 三档,**落在 (0,8) 或 (8,16) 一律整块拒载**。
//   也就是「未来小版本追加字段」只能按**整级**(每级两个 u32)追加 —— 想只尾扩一个 u32
//   而不升 abi 是做不到的,那份 blob 会被自己拒掉。上面那句「解码保留、编码原样回写、
//   防静默丢字段」说的是**尾字段齐了之后**的未知尾部,不是「任意长度都容忍」。
// **不可就地追加字段破坏既有偏移** —— 24B 定长 header(6×u32)之后才允许经长度回退追加尾部;
// 要加字段:① 升容器 abi 走迁移链(本次 [J69/U24] 即 abi=1→2),或
// ② 放 PRMS 的 ValueTree(天生容忍字段增删,STATE_SCHEMA §三 的 ui 组即登记在 PRMS 名下)。
// T37 的 guide_seen / tour_seen 走的是 ②,见 src/output/OutputUiState.h。
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
inline constexpr std::uint32_t kOutputLoudnessModeMax = 2; // [J69/U24①] 序号 0..2
inline constexpr std::uint32_t kOutputCenterSlotPolicyMax = 2; // [J69/U24④] 序号 0..2
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
    std::string loudnessMode = "kw_integrated"; // [J69/U24①] 段响度口径,默认 kw_integrated
    std::string centerSlotPolicy = "priority_queue"; // [J69/U24④] 中心槽策略,默认 priority_queue
    // [SL-279] 「上次全量分析所用」的那一份 —— stale 派生式的另一半(03 §6.3)。
    // 落盘在这里而不是留在内存:UI 此前拿「设置页 mount 时的本地快照」当基线,而 mount 早于
    // 首次 state 到达,于是存了非默认档的工程一进设置页就误报「需重新分析」。
    std::string appliedLoudnessMode = "kw_integrated";
    std::string appliedCenterSlotPolicy = "priority_queue";
    std::vector<std::uint8_t> unknownTail; // 已知字段之后的未知尾部(未来小版本追加;解码保留、编码回写)
    // masterChartMode 不属 CFGS:由独立 UICF chunk(kFourccUiConfig)承载,见 encodeUiConfig/decodeUiConfig。
};

// decode 回落报告:未知/越界枚举序号 → 回落默认并计数(不静默)。
struct OutputDecodeReport
{
    std::uint32_t loudnessModeFallbacks = 0; // 未知/越界 loudness_mode → 默认 次数
    std::uint32_t centerSlotPolicyFallbacks = 0; // 未知/越界 center_slot_policy → 默认 次数
    // [SL-279] applied.* 单独计数,**不与上面两个合并** —— 合并之后 DBG 那行会说
    // 「loudness_mode 回落了 1 次」,而实际回落的是 applied 那一份,把人指到错的字段上。
    std::uint32_t appliedLoudnessModeFallbacks = 0;
    std::uint32_t appliedCenterSlotPolicyFallbacks = 0;
};

// 编码;语言超长截断(≤kOutputLanguageMaxBytes)。返回 false = 无法分配。
bool encodeOutputState(const OutputState& s, std::vector<std::uint8_t>& out);

// 解码;严格校验(长度/范围)。失败 → 返回 false 且 out 保持默认(不半填充)。
// report 非空时写回落计数(未知/越界枚举序号 → 默认);nullptr 忽略。
bool decodeOutputState(const std::uint8_t* data, std::size_t size, OutputState& out,
                       OutputDecodeReport* report = nullptr);

// [J75] T43:ui.master_chart_mode 的独立 UICF chunk 载荷编解码(定长 4 字节 u32)。
// encode:恒写 4 字节(0=distribution | 1=trajectory)。
// decode:长度 != 4 → 返回 false(§7.3 拒载,调用方回落默认);未知取值(≥2)→ 回落默认 distribution。
bool encodeUiConfig(std::uint32_t masterChartMode, std::vector<std::uint8_t>& out);
bool decodeUiConfig(const std::uint8_t* data, std::size_t size, std::uint32_t& out);

} // namespace scvb::state
