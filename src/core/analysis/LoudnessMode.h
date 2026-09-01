// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace scvb::analysis
{

// 段响度口径选项集 analysis.loudness_mode(02-dsp-spec §4.3,J69/U24①)。
// 三档只切换「第二响度指标」口径;主口径 L_seg(ADR-009/[J18] 冻结)不随 mode 变化。
// 选项集与每档数学真源 = 02 §4.3(设计稿三选 LUFS-S/RMS/峰值 dBFS 只是视觉呈现)。
enum class LoudnessMode
{
    KIntegrated = 0, // 默认:K 加权段积分(ungated,窗长=段长)
    Rms = 1, // RMS 平均(幅度域算术平均,仍带 K 加权)
    PeakDbfs = 2, // 峰值 dBFS(未加权样本峰值,非 true-peak)
};

// 枚举 → 桥面真值 UTF-8 字符串(SCVB_CONTRACT §1.21/§9.2;KIntegrated → "kw_integrated")。
// 注:core 历史拼写 "k_integrated" 仅由 parseLoudnessMode 兼容接受,序列化一律出 "kw_integrated"。
const char* loudnessModeToString(LoudnessMode mode);

struct LoudnessModeParseResult
{
    LoudnessMode mode = LoudnessMode::KIntegrated;
    bool fellBack = false; // 未知/越界 → 回落默认档
    const char* warning = nullptr; // 回落原因(记 warning,不静默);未回落为 nullptr
};

// 解析 state 字段(UTF-8 字符串)。未知串 → 回落默认档 + warning(03 §6.3:原串回写保留)。
LoudnessModeParseResult parseLoudnessMode(const char* value);

// 解析整型枚举序号(越界 int → 回落默认档 + warning)。
LoudnessModeParseResult parseLoudnessMode(int ordinal);

// 主口径 L_seg(02 §4.1 / ADR-009 / [J18] 冻结:ungated,窗长=段长)。
// z = (1/N)·Σ kw_ms[k];L = −0.691 + 10·log10(max(z, 1e−12))。
// **不接收 mode 参数** —— 切换 loudness_mode 不改变主口径(逐位不变)。
double computeLseg(const float* kwMs, std::size_t n);

// 第二响度指标(02 §4.3),按 mode 求值。kwMs = K-weighted mean-square;
// peak = 未加权样本峰值(仅 PeakDbfs 档使用,可为 nullptr,其余档忽略)。
// ---------------------------------------------------------------------------
// [SL-252 / J95②a] **这三个 mode-aware 函数服务的是「第二响度指标读数」那条路,
// 不是平衡归一化基准 z。** 修宪 ADR-009 v2.2 把两件事分开了:
//   · 上报段响度 L_seg(澄清 ①)—— 恒为 ungated K-weighted 积分,不随 mode 变;
//   · 平衡归一化基准 z(澄清 ②)—— 按 mode 选档,落点是 `analysis/BalanceBasis.h`
//     的 `balanceBasisZ`,**返回线性能量**。
// 本文件这几个返回的是 **dB**,且三档不在同一把尺上(KIntegrated 含 −0.691 偏移走
// 10·log10 的能量域,Rms/PeakDbfs 无偏移走 20·log10 的幅度域)——塞进 z 会让 AutoAssign
// 的 `zSum` 变负、`zHat` 失去意义,所以 A 案**没有复用**它们,另写了纯函数。
// **保留不动**:它们是「第二指标读数」尚未落地那条路的既有实现,不是死代码,别顺手删。
// ---------------------------------------------------------------------------
double computeLoudnessMetric(LoudnessMode mode, const float* kwMs, const float* peak, std::size_t n);

// 分析设置失效标记(03 §6.3):analysis_settings_stale := loudness_mode ≠ applied.loudness_mode。
// 置位 = 用户改 mode;清零 = 一次全量分析完成后 markApplied()。只提示、不自动重算(UI 归 T35)。
// T09 承载 loudness_mode 一项;center_slot_policy 由 T13 追加到同一派生式。
struct AnalysisSettingsStale
{
    LoudnessMode loudnessMode = LoudnessMode::KIntegrated; // 当前(用户设置)
    LoudnessMode appliedLoudnessMode = LoudnessMode::KIntegrated; // 上次全量分析所用

    bool stale() const { return loudnessMode != appliedLoudnessMode; }

    void markApplied() { appliedLoudnessMode = loudnessMode; }
};

// 分析链段响度口径求值:读 settings 决定第二指标口径;主口径恒为 computeLseg(不随 settings 变)。
//
// [SL-262] 本类型原名 `SegmentLoudness`,与 `analysis/Loudness.h` 里**同名同命名空间**但
// **成员完全不同**的另一个结构并存(那个是 `{LoudnessValue main; peakMax; MomentaryStats}`)
// —— 按标准是 ODR 违规。改名收敛:`SegmentLoudness` 这个名字留给 Loudness.h 那份**完整测量
// 结果**,本类型按它真正承载的东西命名 —— **两个读数**(主口径 + 第二指标)。
struct SegmentLoudnessReadings
{
    double lseg = 0.0; // 主口径 L_seg(ADR-009/[J18] 冻结),恒等于 computeLseg(kwMs, n)
    double lmode = 0.0; // 第二指标,依 settings.loudnessMode(02 §4.3)
};

SegmentLoudnessReadings computeSegmentLoudness(const AnalysisSettingsStale& settings, const float* kwMs,
                                               const float* peak, std::size_t n);

} // namespace scvb::analysis
