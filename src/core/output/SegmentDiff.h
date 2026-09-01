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
//   · **范围**:`kept` 只数**落在本轮分析范围内**的段(半开区间相交:`t1 > r0 ∧ t0 < r1`)。
//     判据与 `previewAnalysis` 的 `manualKept` 逐字同款(那对 `rangeS0/rangeS1` 与
//     `applyAnalysisSegments` 判 `outsideRange` 的是同一对数)—— [SL-193] 修的正是
//     「dry-run 报的 {k} 恒大于事后 diff.kept」,不过滤范围就是从另一侧把它再造一遍:
//     Tab3 同一张卡上 A-07 预览行与 A-02 摘要行会给两个数。
//     `added/removed/changed` 不受影响:范围外的段两侧一一对应、原样保留,天然全 0。
//     **轨维**同理由调用方按「本轮真参与分析的轨」筛(未参与的轨整表恒等,也不该贡献 kept)。
//   · `origin` / `locked` **不进判据**:UI 不渲染它们;且按 [J34] 这条路根本不动用户段。

#include <algorithm>
#include <cstdint>
#include <numeric>
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

// `changed[]` 的**条数上限**(安全阀,不是业务口径)。
//
// 为什么要有:全量重分段时边界一挪,pan/volDb 跨过 1 位小数的段可能数以千计。UI 那侧
// `renderDiffItems` 是**逐条拼 `<li>`、无分页无封顶**的,几千条会连同 juce::var 载荷一起
// 把一条「一次性反馈条」变成几千行 DOM —— 而这条摘要本来就是几秒后自动收起的瞥一眼式反馈。
//
// ⚠ **超出部分是被截掉的,不另发计数**:§2.8 的 `diff` 块字段是冻结的四个
// (kept/added/removed/changed),加一个「还有多少条」的字段属契约变更,不在本卡范围。
// `kept`/`added`/`removed` 三个**仍是精确总数**,只有 `changed[]` 这张明细表被截断。
// 取 200:比 mock 的展示档(8 条)宽得多,又把最坏 DOM 规模钉在三位数。
inline constexpr std::size_t kMaxChangedItems = 200;

// 显示精度:1 位小数(UI 的 `fmtSigned(x, 1)`)。判「用户看得出来的差别」,不是判浮点相等。
// 非有限值先死掉:`std::lround(NaN)` 是**实现定义**行为,而本函数是纯函数、调用方不止一个;
// 判「两个非有限值相同」没有意义,一律按「不同」处理(方向保守:宁可多报一条改动)。
inline bool sameAtDisplayPrecision(float a, float b) noexcept
{
    if (!std::isfinite(a) || !std::isfinite(b))
        return false;
    const auto q = [](float v) { return static_cast<long long>(std::lround(static_cast<double>(v) * 10.0)); };
    return q(a) == q(b);
}

inline std::int64_t overlapOf(const scvb::state::Segment& a, const scvb::state::Segment& b) noexcept
{
    const std::int64_t lo = std::max(a.t0, b.t0);
    const std::int64_t hi = std::min(a.t1, b.t1);
    return hi > lo ? (hi - lo) : 0;
}

// 两张段表**逐字节相同**吗(五个字段全等)。
//
// 用途:`finishAnalysis` 判「这一轮到底改没改」—— 改了才压撤销步。**不能拿 SegmentDiff
// 是否全空来代替**:`changed` 只比 pan/volDb,边界挪了而两个值没变的重分段,diff 全空
// 但段表真变了,漏压这一步用户就撤不回来。
//
// 浮点用精确 `==`(问的是「有没有变」,不是「看得出来吗」)。NaN != NaN ⇒ 判不同 ⇒
// 照压事务,方向保守(宁可多一条撤销步,不可少一条)。
inline bool segmentsIdentical(const std::vector<scvb::state::Segment>& a,
                              const std::vector<scvb::state::Segment>& b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].t0 != b[i].t0 || a[i].t1 != b[i].t1 || a[i].flags != b[i].flags)
            return false;
        if (!(a[i].pan == b[i].pan) || !(a[i].volDb == b[i].volDb))
            return false;
    }
    return true;
}

// 单轨比对,结果并入 out。
//
// `ch` 只用来填条目里的轨号;`rangeStartSample/rangeEndSample` = 本轮分析的**量化后**范围
// (调用方直接把 `applyAnalysisSegments` 收到的那一对原样传进来,三处判据同源)。
//
// **复杂度**:配对走「两侧按 t0 有序 + 单调游标」的线性扫,不是逐对枚举 ——
// 本函数在 `finishAnalysis` 里对 15 轨全程持 `lifecycleMutex_` 跑在**消息线程**上,
// 密集工程(2000 段/轨)的 O(n²) 会在 [M] 上吃掉几十毫秒(与 [SL-232] 在同一个
// `finishAnalysis` 里修过的「P0-A 冻死」同族)。
//
// 两张表在生产侧都已按 t0 有序且互不重叠,但本函数**不对调用方提这个要求**:自建有序
// 下标(`std::sort` 对已有序输入近乎线性),排序后语义与逐对枚举的贪心逐字等价 ——
// 非重叠段的重叠量为 0,而选优判据是严格 `>`,零重叠者本就永远选不上。
inline void diffTrackInto(int ch, const std::vector<scvb::state::Segment>& oldSegs,
                          const std::vector<scvb::state::Segment>& newSegs, std::int64_t rangeStartSample,
                          std::int64_t rangeEndSample, SegmentDiff& out)
{
    const auto byT0 = [](const scvb::state::Segment& x, const scvb::state::Segment& y) {
        return x.t0 != y.t0 ? x.t0 < y.t0 : x.t1 < y.t1;
    };
    std::vector<std::size_t> oldOrd(oldSegs.size());
    std::iota(oldOrd.begin(), oldOrd.end(), std::size_t{0});
    std::sort(oldOrd.begin(), oldOrd.end(), [&](std::size_t x, std::size_t y) { return byT0(oldSegs[x], oldSegs[y]); });
    std::vector<std::size_t> newOrd(newSegs.size());
    std::iota(newOrd.begin(), newOrd.end(), std::size_t{0});
    std::sort(newOrd.begin(), newOrd.end(), [&](std::size_t x, std::size_t y) { return byT0(newSegs[x], newSegs[y]); });

    std::vector<bool> oldUsed(oldSegs.size(), false);
    std::size_t lo = 0; // oldOrd 里第一条「还可能与后续新段重叠」的位置(只进不退)
    for (std::size_t k = 0; k < newOrd.size(); ++k)
    {
        const std::size_t ni = newOrd[k];
        const auto& nn = newSegs[ni];
        // 整条落在 nn 起点之前的旧段,与**后续**新段也不可能重叠(新段起点单调不减)。
        while (lo < oldOrd.size() && oldSegs[oldOrd[lo]].t1 <= nn.t0)
            ++lo;

        // 取重叠最大的那一条旧段;**每条旧段只配一次**(否则一条旧段被两条新段共用,
        // removed 会少数、added 会多数,两边都不守恒)。
        std::size_t best = oldSegs.size();
        std::int64_t bestOv = 0;
        for (std::size_t i = lo; i < oldOrd.size(); ++i)
        {
            const std::size_t oi = oldOrd[i];
            if (oldSegs[oi].t0 >= nn.t1)
                break; // 有序 ⇒ 再往后只会更靠右,不必看
            if (oldUsed[oi])
                continue;
            const std::int64_t ov = overlapOf(nn, oldSegs[oi]);
            if (ov > bestOv)
            {
                bestOv = ov;
                best = oi;
            }
        }
        if (best == oldSegs.size())
        {
            ++out.added; // 没有任何旧段与之重叠
            continue;
        }
        oldUsed[best] = true;
        const auto& o = oldSegs[best];
        if (!(sameAtDisplayPrecision(o.pan, nn.pan) && sameAtDisplayPrecision(o.volDb, nn.volDb)) &&
            out.changed.size() < kMaxChangedItems)
        {
            out.changed.push_back(SegmentDiffItem{ch, static_cast<int>(ni), o.pan, nn.pan, o.volDb, nn.volDb});
        }
    }
    for (std::size_t o = 0; o < oldSegs.size(); ++o)
    {
        if (!oldUsed[o])
            ++out.removed;
    }
    // kept = 本轮**范围内**保留下来的手动编辑 / 锁定段(契约 §2.8 逐字;见头注)。
    // 数新表 —— 这条路按 [J34] 根本不动用户段,数哪一边都一样,数新表更贴「现在还在的有几段」。
    for (const auto& sg : newSegs)
    {
        if (!(sg.t1 > rangeStartSample && sg.t0 < rangeEndSample))
            continue; // 范围外:本轮压根没碰它,不进「本次保留」的账
        if (scvb::state::segmentLocked(sg.flags) ||
            scvb::state::segmentOrigin(sg.flags) != scvb::state::SegmentOrigin::Auto)
        {
            ++out.kept;
        }
    }
}

} // namespace scvb::output
