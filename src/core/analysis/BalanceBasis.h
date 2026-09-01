// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BalanceBasis —— 平衡层的**归一化基准 z**(ADR-009 v2.2 [J95②→ADR-009 澄清])。
//
// 修宪把两件事分开了,这个头文件只管第二件:
//   ① **上报段响度 L_seg** —— ADR-009 正文第一条,「段内 ungated K-weighted 积分响度」,
//      **不随 `analysis.loudness_mode` 变化**(J18 对拍口径同样不变)。
//      它的落点是 `AnalysisPipeline` 里 `as.energyLinear` / `as.loudnessLufs` 那一支,不在本文件。
//   ② **平衡归一化基准 z** —— 由 `analysis.loudness_mode` 选择,就是本文件。
//
// **为什么不复用 `computeLoudnessMetric`**(SL-252 定谳):`AutoAssign` 对 z 的用法是
// `zSum += t.z` / `zHat = t.z / zSum` / `zL = t.z / 2` / `lCoef = c²·cos²θ·t.z` ——
// 必须是**非负、可加、可求比的线性能量量**。而那个函数三档返回的全是 **dB(负数)**,
// 且**不在同一把尺上**:`KIntegrated` 含 `kLufsOffset = −0.691` 走 `10·log10`(能量域),
// `Rms`/`PeakDbfs` 无偏移、走 `20·log10`(幅度域)。塞进 z 会让 `zSum` 变负、`zHat` 失去意义。
// 那三个既有函数服务的是「第二指标读数」那条尚未落地的路,保留不动(见其头注)。
//
// 三档 z **均为线性能量量**,故 ADR-009 正文第三条「所有平衡计算在线性能量域做」
// **不受影响、继续完整适用** —— 这正是修宪只加澄清、不改第三条的原因。
//
// **默认档必须逐位不变**:`KIntegrated` 直接调 `meanKw`,与本次修订前**同一个函数、
// 同一条代码路径**,不得实现成 `10^(L/10)` 之类的等价换算(那会引入浮点误差,让既有工程
// 重分析后 pan/volDb 发生肉眼不可见但逐位不同的漂移)。用例以此反向钉住。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "analysis/LoudnessMode.h"

namespace scvb::analysis
{

// 段内平均 K 加权能量 → 线性能量(§4 的总能量口径)。
// 窗口 [begin, end) 按 kw 的实际长度夹取;空窗返回 0.0。
inline double meanKw(const std::vector<float>& kw, std::int64_t begin, std::int64_t end)
{
    if (end <= begin)
    {
        return 0.0;
    }
    const std::int64_t n = static_cast<std::int64_t>(kw.size());
    const std::int64_t b = std::max<std::int64_t>(0, begin);
    const std::int64_t e = std::min<std::int64_t>(n, end);
    if (e <= b)
    {
        return 0.0;
    }
    double acc = 0.0;
    for (std::int64_t i = b; i < e; ++i)
    {
        acc += static_cast<double>(kw[static_cast<std::size_t>(i)]);
    }
    return acc / static_cast<double>(e - b);
}

// 平衡归一化基准 z,按档求值。**返回线性能量**(不是 dB),恒非负。
//   · `kw_integrated`(默认)= `mean(kw)`     —— 与修订前逐位相同(同一个 meanKw);
//   · `rms`                  = `(mean(√kw))²` —— 幅度域取平均再回能量域;
//   · `peak_dbfs`            = `max(peak)²`   —— 未加权样本峰值的能量当量。
// `peak` 为空或长度不足时,`peak_dbfs` 档按「该窗无峰值数据」得 0.0(与 kw 全零同义,
// 下游 `zSum` 为 0 时 AutoAssign 已有的零能量分支接住,不额外造分支)。
inline double balanceBasisZ(LoudnessMode mode, const std::vector<float>& kw, const std::vector<float>& peak,
                            std::int64_t begin, std::int64_t end)
{
    switch (mode)
    {
    case LoudnessMode::KIntegrated:
        return meanKw(kw, begin, end);

    case LoudnessMode::Rms: {
        if (end <= begin)
        {
            return 0.0;
        }
        const std::int64_t n = static_cast<std::int64_t>(kw.size());
        const std::int64_t b = std::max<std::int64_t>(0, begin);
        const std::int64_t e = std::min<std::int64_t>(n, end);
        if (e <= b)
        {
            return 0.0;
        }
        double sumAmp = 0.0;
        for (std::int64_t i = b; i < e; ++i)
        {
            // kw 是能量(≥ 0),√kw 即幅度;负值只可能来自坏数据,夹到 0 免得 sqrt 出 NaN。
            const double v = static_cast<double>(kw[static_cast<std::size_t>(i)]);
            sumAmp += std::sqrt(v > 0.0 ? v : 0.0);
        }
        const double meanAmp = sumAmp / static_cast<double>(e - b);
        return meanAmp * meanAmp; // 回到能量域 —— z 必须与另两档同量纲才可加可求比
    }

    case LoudnessMode::PeakDbfs: {
        if (end <= begin)
        {
            return 0.0;
        }
        const std::int64_t n = static_cast<std::int64_t>(peak.size());
        const std::int64_t b = std::max<std::int64_t>(0, begin);
        const std::int64_t e = std::min<std::int64_t>(n, end);
        if (e <= b)
        {
            return 0.0;
        }
        double maxPeak = 0.0;
        for (std::int64_t i = b; i < e; ++i)
        {
            // 与 Rms 支同口径:负值 / NaN 只可能来自坏数据,夹到 0,别让它污染 z。
            // 用 `v > maxPeak` 而不是 `std::max`:前者对 NaN 恒假、天然把 NaN 挡在外面,
            // 而 `std::max(0.0, NaN)` 返回哪一个是实现决定的,不是标准保证。
            const double v = static_cast<double>(peak[static_cast<std::size_t>(i)]);
            if (v > maxPeak)
            {
                maxPeak = v;
            }
        }
        return maxPeak * maxPeak; // 幅度 → 能量当量
    }
    }
    return meanKw(kw, begin, end); // 枚举越界:回默认档(与 computeLoudnessMetric 同款兜底)
}

} // namespace scvb::analysis
