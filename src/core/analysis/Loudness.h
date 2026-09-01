// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace scvb::analysis
{

// 段响度口径(02-dsp-spec §4.3 / J69 / U24①)。
// 三档全部由现有 hop 特征(kw_ms / peak,ipc v1.5 冻结)离线可算,切换不需重采集。
enum class LoudnessMode
{
    kIntegrated, // 默认:K 加权段积分(ungated,窗长=段长)
    kRms, // RMS 平均(幅度域算术平均)
    kPeakDbfs, // 峰值 dBFS(未加权样本峰值)
};

// 段响度主指标(§4.1/§4.3)。
// z     = 线性等效能量,下游 §5/§6 平衡计算的输入;增益 u dB ⇒ z → z·10^(u/10)。
//         k_integrated 档 z = (1/N)·Σ kw_ms(线性能量,与 LOUD-1/5 向量一致);
//         rms / peak_dbfs 档 z = 10^(L/10)。
// level = 主指标 L_mode(LUFS;k_integrated / rms 为 K 加权口径,peak_dbfs 为 dBFS)。
struct LoudnessValue
{
    double z = 0.0;
    double level = 0.0;
};

// 段附带指标(§4.1,恒按 K 加权口径生成,不随 mode 变化)。
struct MomentaryStats
{
    double p95 = 0.0; // 400ms=40hop 滑窗 K 加权响度 p95(线性插值分位)
    double max = 0.0; // 滑窗 K 加权响度极值
    double min = 0.0;
};

// 单段响度完整结果(主指标 + 附带指标)。
struct SegmentLoudness
{
    LoudnessValue main;
    double peakMax = 0.0; // 段内未加权样本峰值最大值(特征域 max_k peak[k])
    MomentaryStats momentary;
};

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
// 仅求主指标(§4.1/§4.3);kwMs/peak 为段内 hop 特征序列,长度 numHops(>0)。
// 纯函数、确定性:同输入同 platform 逐位一致,与块切分无关(切分一致性由特征层保证)。
LoudnessValue segmentLoudness(const float* kwMs, const float* peak, int numHops, LoudnessMode mode);

// 求完整段结果(主指标 + peakMax + 400ms 滑窗 p95/max/min)。
SegmentLoudness measureSegment(const float* kwMs, const float* peak, int numHops, LoudnessMode mode);

// 400ms 滑窗的 hop 数(10ms hop × 40,ADR-009 冻结 M 口径)。
inline constexpr int kMomentaryWindowHops = 40;

} // namespace scvb::analysis
