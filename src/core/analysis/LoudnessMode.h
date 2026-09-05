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

// 分析设置失效标记(03 §6.3)**已搬到 `analysis/AnalysisSettings.h`**。
// [SL-278/SL-279] 这里原来写着「T09 承载 loudness_mode 一项;center_slot_policy 由 T13 追加到
// 同一派生式」—— **本卡就是那笔追加的兑现方**,所以那句宣称连同结构一起不留在这里。
// 搬走的理由:结构现在要同时装 `LoudnessMode` 与 `CenterSlotPolicy`,而 `CenterSlotPolicy`
// 的家 `AutoAssign.h` 已经 include 了本文件 —— 留在这里就成环。
// 为什么单开一个头、两项为什么各判各的,都写在那个头里,这里不抄第二份。

// 分析链段响度口径求值:按 mode 决定第二指标口径;主口径恒为 computeLseg(不随 mode 变)。
//
// [SL-262] 本类型原名 `SegmentLoudness`,与 `analysis/Loudness.h` 里**同名同命名空间**但
// **成员完全不同**的另一个结构并存(那个是 `{LoudnessValue main; peakMax; MomentaryStats}`)
// —— 按标准是 ODR 违规。改名收敛:`SegmentLoudness` 这个名字留给 Loudness.h 那份**完整测量
// 结果**,本类型按它真正承载的东西命名 —— **两个读数**(主口径 + 第二指标)。
struct SegmentLoudnessReadings
{
    double lseg = 0.0; // 主口径 L_seg(ADR-009/[J18] 冻结),恒等于 computeLseg(kwMs, n)
    double lmode = 0.0; // 第二指标,依 mode(02 §4.3)
};

// [SL-279] 入参从 `const AnalysisSettingsStale&` **收成 `LoudnessMode`**:本函数只读那个结构的
// `loudnessMode` 一个成员,收窄之后本文件不必再认识那个结构(否则就是上面说的那个环)。
// 生产侧零调用点(这条「第二指标读数」的路尚未落地,见本文件上方 [SL-252] 那段),
// 调用点只有 `tests/core/test_vad.cpp` 两处。
SegmentLoudnessReadings computeSegmentLoudness(LoudnessMode mode, const float* kwMs, const float* peak, std::size_t n);

} // namespace scvb::analysis
