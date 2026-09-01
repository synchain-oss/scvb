// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SegmentDiff —— 段表前后比对(§2.8 载荷的 `diff` 块;[SL-255])。
//
// 为什么需要它:§1.18/§1.19 的语义行写着「完成后回发 `scvb.segments`(`reason:"vad"`,
// **含 diff 摘要**)」,而 native 那个 `diff` 块一直是**硬编码的桩**(全 0),从来没算过。
// Tab3 的 A-02 摘要条正是对 `reason ∈ {vad, segmentation, analyze}` 读它 —— 于是
// 「拖了没效果」修好之后会变成「有效果但摘要说改了 0 段」。承诺没兑现,换个方式再违约一次。
//
// 纯函数、JUCE-free,scvb_tests 可直接断言。
//
// ## 判同口径(与 UI 的渲染面对齐,别自造)
//
// `changed[]` 的条目形状由 UI 唯一决定(`tab-wave.js::renderDiffItems`):
// `{ch, segIdx, panFrom, panTo, volDbFrom, volDbTo}` —— 它**只渲染 pan/volDb 的前后值**。故:
//
//   · **配对**:old / new 两张表按**时间重叠最大**的那一条配对。不用 (t0,t1) 全等 ——
//     重分段本来就会挪边界,全等配对会把几乎所有段判成「删一条 + 加一条」,摘要就没信息量;
//     重叠配对表达的是「这一段音频」,与用户看波形时的直觉一致。
//   · **changed**:配对上、且 pan 或 volDb 按**显示精度(1 位小数)**不同 —— 判据与用户
//     屏幕上看到的数字对齐,亚显示精度的浮点抖动不算改动。`segIdx` 取**新表**的下标
//     (UI 显示 `segIdx+1` =「第 N 段」,指的就是现在这张表的第几段)。
//   · **added / removed**:新表里没配上的条数 / 旧表里没配上的条数。
//   · **kept**:⚠ **不是**「没改动的条数」。契约 §2.8 字段纪律逐字:「`diff.kept` = 本次
//     **手动编辑/锁定段已保留**计数(05 词条 `wave.diffKept` 的 `{k}`)」,词条原文是
//     「{k} 处手动编辑/锁定段已保留」。所以它数的是**新表里 `locked || origin != Auto`
//     的段数** —— 回答的是「你手改过的那些段这次动没动」,不是「有多少段值没变」。
//     (这一格我第一版写成了「配对上且未改动」,读契约才发现是两回事。)
//   · `origin` / `locked` **不进判据**:UI 不渲染它们;且按 [J34] 这条路根本不动用户段。

#include <algorithm>
#include <cstdint>
#include <vector>

#include <cmath>

#include "state/StateCodec.h"

namespace scvb::output
{

struct SegmentDiffItem
{
    int ch = 0; // 1..15
    int segIdx = 0; // **新表**下标(0 基)
    float panFrom = 0.0f;
    float panTo = 0.0f;
    float volDbFrom = 0.0f;
    float volDbTo = 0.0f;
};

struct SegmentDiff
{
    int kept = 0;
    int added = 0;
    int removed = 0;
    std::vector<SegmentDiffItem> changed;
};

// 显示精度:1 位小数(UI 的 `fmtSigned(x, 1)`)。判「用户看得出来的差别」,不是判浮点相等。
inline bool sameAtDisplayPrecision(float a, float b) noexcept
{
    const auto q = [](float v) { return static_cast<long long>(std::lround(static_cast<double>(v) * 10.0)); };
    return q(a) == q(b);
}

inline std::int64_t overlapOf(const scvb::state::Segment& a, const scvb::state::Segment& b) noexcept
{
    const std::int64_t lo = std::max(a.t0, b.t0);
    const std::int64_t hi = std::min(a.t1, b.t1);
    return hi > lo ? (hi - lo) : 0;
}

// 单轨比对,结果并入 out。`ch` 只用来填条目里的轨号。
inline void diffTrackInto(int ch, const std::vector<scvb::state::Segment>& oldSegs,
                          const std::vector<scvb::state::Segment>& newSegs, SegmentDiff& out)
{
    std::vector<bool> oldUsed(oldSegs.size(), false);
    for (std::size_t n = 0; n < newSegs.size(); ++n)
    {
        // 取重叠最大的那一条旧段;**每条旧段只配一次**(否则一条旧段被两条新段共用,
        // removed 会少数、added 会多数,两边都不守恒)。
        std::size_t best = oldSegs.size();
        std::int64_t bestOv = 0;
        for (std::size_t o = 0; o < oldSegs.size(); ++o)
        {
            if (oldUsed[o])
                continue;
            const std::int64_t ov = overlapOf(newSegs[n], oldSegs[o]);
            if (ov > bestOv)
            {
                bestOv = ov;
                best = o;
            }
        }
        if (best == oldSegs.size())
        {
            ++out.added; // 没有任何旧段与之重叠
            continue;
        }
        oldUsed[best] = true;
        const auto& o = oldSegs[best];
        const auto& nn = newSegs[n];
        if (!(sameAtDisplayPrecision(o.pan, nn.pan) && sameAtDisplayPrecision(o.volDb, nn.volDb)))
        {
            out.changed.push_back(SegmentDiffItem{ch, static_cast<int>(n), o.pan, nn.pan, o.volDb, nn.volDb});
        }
    }
    for (std::size_t o = 0; o < oldSegs.size(); ++o)
    {
        if (!oldUsed[o])
            ++out.removed;
    }
    // kept = 保留下来的**手动编辑 / 锁定**段(契约 §2.8 逐字;见头注)。数新表 ——
    // 这条路按 [J34] 根本不动用户段,数哪一边都一样,数新表更贴「现在还在的有几段」。
    for (const auto& sg : newSegs)
    {
        if (scvb::state::segmentLocked(sg.flags) ||
            scvb::state::segmentOrigin(sg.flags) != scvb::state::SegmentOrigin::Auto)
        {
            ++out.kept;
        }
    }
}

} // namespace scvb::output
