// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>

// HopMath —— 「采样点 → hop」的**唯一换算口径**([SL-262])。
//
// 单拎成纯函数的理由与同族的 `AnalyzeScopeMath.h` 一脉相承:这条换算原先在两处**各写了
// 一遍** —— `AnalysisPipeline.cpp` 的分析入口,和 `OutputProcessor::segmentLoudnessLufs`
// 的上报路径。后者挂在 `ScvbOutputAudioProcessor` 上,`scvb_tests` 够不着,于是
// 「把 `t0 / hopSamples` 改回秒往返」这类回归**测试照样绿**(#169 复审【重要】②;
// 判例逐字见 `AnalyzeScopeMath.h` 头注)。抽出来两件事一起解决:两处共用同一份口径
// (同一语义两个落点,§0.1 第 4 条 —— 那正是 SL-262 这张卡在偿还的债),
// 且 `scvb_tests` 能直接把边界钉死。
//
// **为什么必须整型**:秒→hop 走浮点除法时,段边界恰落 hop 边界的情况约 **4.9%** 会截断到
// k−1(实测 1..200000 个边界里 9721 次,例 `29*0.01/0.01 = 28.999999999999996 → 28`),
// 把段尾最后一个 hop 排除出窗口。整型除法没有这个问题。

namespace scvb::analysis
{

// hop 的采样点数。`hopSeconds` 是 feat 段的几何常量(不是采样率派生量),`sampleRate`
// 非正时按 48000 兜底 —— 与 `OutputProcessor::prepareToPlay` 的
// `sampleRate > 0.0 ? sampleRate : 48000.0` 同款,不在这里造第二套兜底。
// 返回 ≤ 0 表示换算不成立(hopSeconds 非正),调用方据此早退。
inline std::int64_t hopSamplesFor(double hopSeconds, double sampleRate)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    if (!(hopSeconds > 0.0))
    {
        return 0;
    }
    return static_cast<std::int64_t>(std::llround(hopSeconds * sr));
}

struct HopWindow
{
    std::uint64_t first = 0;
    std::uint64_t last = 0; // 半开:[first, last)
    bool valid = false; // false = 窗不成立(空窗/倒序/换算不成立),调用方按静音处理
};

// 采样点半开区间 [t0Samples, t1Samples) → hop 半开区间。负采样点夹到 0。
inline HopWindow hopWindowFromSamples(std::int64_t t0Samples, std::int64_t t1Samples, double hopSeconds,
                                      double sampleRate)
{
    HopWindow w;
    const std::int64_t hopSamples = hopSamplesFor(hopSeconds, sampleRate);
    if (hopSamples <= 0 || t1Samples <= t0Samples)
    {
        return w;
    }
    const std::int64_t lo = t0Samples > 0 ? t0Samples : 0;
    const std::int64_t hi = t1Samples > 0 ? t1Samples : 0;
    w.first = static_cast<std::uint64_t>(lo / hopSamples);
    w.last = static_cast<std::uint64_t>(hi / hopSamples);
    w.valid = w.last > w.first;
    return w;
}

} // namespace scvb::analysis
