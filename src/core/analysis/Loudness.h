// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

// [SL-262] `LoudnessMode` 的**唯一定义**在 `analysis/LoudnessMode.h`;本文件只是 include 它。
// 本文件此前自带一份同名同命名空间的枚举
// (拼法 kIntegrated/kRms/kPeakDbfs),与 `LoudnessMode.h` 的 KIntegrated/Rms/PeakDbfs
// 并存 —— 同一命名空间两个定义,按标准是 ODR 违规,靠「没有哪个 TU 同时 include 两头」
// 侥幸不炸。SL-252 让 `AutoAssign.h`(公开头)include 了 LoudnessMode.h 后传播面扩大,
// 本卡收敛成单一定义。保留的是 LoudnessMode.h 那份:它带 `parseLoudnessMode` /
// `loudnessModeToString`(桥面 §1.21/§9.2 的真源)且有显式序号,序号与旧拼法逐一对应。
#include "analysis/LoudnessMode.h"

namespace scvb::analysis
{

// 段响度主指标(§4.1/§4.3)。
// z     = 线性等效能量;增益 u dB ⇒ z → z·10^(u/10)。
//         ⚠ **平衡层用的归一化基准 z 不走这里** —— 那条路由 ADR-009 v2.2 [J95②→ADR-009 澄清]
//         定义、落点是 `analysis/BalanceBasis.h` 的 `balanceBasisZ`(SL-252)。本字段是
//         `segmentLoudness` 自己的读数;`rms`/`peak_dbfs` 档在这里走 `10^(L/10)` 的 dB 往返,
//         与 BalanceBasis 的直接式**数学等价但浮点不逐位相同**,别把两者混用。
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
