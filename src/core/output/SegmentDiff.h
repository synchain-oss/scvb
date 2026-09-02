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
//   · **changed**:配对上、且 pan 或 volDb 的改动**幅度值得一提** —— 判据 =
//     `changedAtDisplayPrecision`:量化到 1 位小数后不同(屏幕上的数字真的变了)
//     **且**幅度过 `kChangeAmplitudeGate`(= **半个**显示步长 0.05,即这个「不同」
//     不是靠贴着量化边界凑出来的)。两条缺一不可;闸门为什么是半步长而不是一整步长
//     (float 下真差一档常常只有 0.099998),理由见该函数头注([SL-274])。
//     `segIdx` 取**新表**的下标
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
// `renderDiffItems` 是逐条拼 `<li>` 的,几千条会连同 juce::var 载荷一起把一条
// 「一次性反馈条」变成几千行 DOM —— 而这条摘要本来就是几秒后自动收起的瞥一眼式反馈。
// ([SL-274] 起 UI 另有两道:明细默认折进 `<details>`、展开后 `max-height` 封顶自滚。
//  那两道管的是**可视高度**,管不了 DOM 条数与载荷大小 —— 本封顶仍然是唯一的那道。)
//
// ⚠ **超出部分是被截掉的,不另发计数**:§2.8 的 `diff` 块字段是冻结的四个
// (kept/added/removed/changed),加一个「还有多少条」的字段属契约变更,不在本卡范围。
// `kept`/`added`/`removed` 三个**仍是精确总数**,只有 `changed[]` 这张明细表被截断。
// 取 200:把最坏 DOM 规模钉在三位数。([SL-274] 起 `web/shared/mock-data.js` 里
// `changed` 的封顶(字面量,无常量名)与 web 侧
// `tab-wave.js::DIFF_CHANGED_CAP` 也是 200 —— **三处同值**,改一处要三处一起改;
// web 侧拿它判「这一帧顶到封顶没有」,顶到就把计数渲染成「200+」而不是「200」。)
//
// ⚠ 封顶是**全局**的(`out` 跨 15 轨累积),而调用方按轨号顺序逐轨调用 —— 所以截断也
// **按轨号顺序**发生:轨 1 自己就改满 200 条时,轨 2..15 一条明细都进不来([SL-255] 复审②)。
// 条目是带轨号渲染的,用户会因此看到「只有 1 轨有改动」的错觉。没有改成按轨配额:
// `added/removed/kept` 三个总数仍如实反映全局改动量,而这条摘要本就是几秒后自动收起的
// 瞥一眼式反馈;真要均摊,得先定「配额怎么分、余数给谁」的呈现口径,那该单独立卡。
inline constexpr std::size_t kMaxChangedItems = 200;

// 显示精度:1 位小数(UI 的 `fmtSigned(x, 1)`)。判「屏幕上那个数一样吗」,不是判浮点相等。
// 非有限值先死掉:取整对 NaN 是**实现定义**行为,而本函数是纯函数、调用方不止一个;
// 判「两个非有限值相同」没有意义,一律按「不同」处理(方向保守:宁可多报一条改动)。
//
// ⚠ **取整方向必须与 UI 同向,别换回 `std::lround`**([SL-274] 复审):本函数量的是
// 「用户屏幕上那个数」,而屏幕上那个数由 `tab-wave.js::fmtSigned` 的 `Math.round` 决定。
// 两者在**负的半格**上分歧:`Math.round` 一律朝 +∞(`-42.5 → -42`),`std::lround` 是
// 远离零(`-42.5 → -43`)。`-4.25f` 在 float 里可精确表示,所以这不是纯理论角 ——
// 用 lround 时 `-4.20f → -4.25f` 屏幕上两边都写 `-4.2`,本函数却判「不同」,于是摘要里
// 冒出一条 `pan -4.2→-4.2` 的**空条目**(正是本卡要消灭的那类);反过来
// `-4.25f → -4.30f` 屏幕上 `-4.2 → -4.3` 明明变了,却被判「相同」而**整条吞掉**。
// `floor(v*10 + 0.5)` 与 `Math.round` 逐值同向,两头的缝一起堵上。用例钉着这两对。
inline bool sameAtDisplayPrecision(float a, float b) noexcept
{
    if (!std::isfinite(a) || !std::isfinite(b))
        return false;
    const auto q = [](float v) { return static_cast<long long>(std::floor(static_cast<double>(v) * 10.0 + 0.5)); };
    return q(a) == q(b);
}

// 显示步长(pan / volDb 都按 1 位小数渲染,`fmtSigned(x, 1)`)。
inline constexpr double kDisplayStep = 0.1;

// 幅度闸门 = **半个**显示步长。为什么是半步长见 `changedAtDisplayPrecision` 头注。
inline constexpr double kChangeAmplitudeGate = kDisplayStep / 2.0;

// 「显示值变了 **且** 变化量值得一提」的改动 ——[SL-274]。
//
// **为什么 `sameAtDisplayPrecision` 一个人不够**:它问的是「四舍五入到 1 位小数之后
// 两个数还一样吗」,那是**量化比较**,不是**幅度比较**。跨在量化边界两侧的一对值,
// 差 0.002 也会被判成「改了」:`4.249 → 4.251` 量化成 `42 → 43`,进 changed[]。
// 这一侧的误报正是用户 v5.6.5 实测「摘要弹出全轨全段」里那批条目的来源。
//
// 判据于是要**两个条件都成立**:量化后不同(屏幕上的数字真的变了)**且**幅度过闸
// (这个「不同」不是靠贴着量化边界凑出来的)。
//
// ⚠ **别把本函数读成「用户看不见的才滤掉」**([SL-274] 复审):`4.249 → 4.251` 在屏幕上
// 读出来是 `4.2 → 4.3`,**数字确实变了**,滤掉它是因为**变化量小到不值一提**。所以本函数
// 的真实语义是「显示值变了 **且** 变化量值得一提」,而不是「用户看得见」—— 按现判据,
// **存在屏幕上数字变了却不进摘要的段**,这是有意为之:这条摘要是几秒后自动收起的
// 瞥一眼式反馈,不是审计日志,把贴着量化边界的抖动全列出来正是原缺陷。
// 下一个人若按「用户看得见」的字面意思去「修」它,就会把闸门删掉、把缺陷放回来。
//
// **闸门为什么是半个步长、不是一个步长 —— 这一条是踩出来的,别调回去。**
// 输入是 `float`,而 0.1 在二进制里不可表示:真的差了一整档的一对值,相减常常落在
// 0.1 **之下**。实测 `-100.0f → -99.9f` 得 0.099998474,`-20.0f → -19.9f`、
// `5.9f → 6.0f`、`0.3f → 0.4f` 同款。pan 值域 −100..100 的 2000 对相邻档里有
// **1188 对**、volDb 值域 −24..12 的 360 对里有 **164 对**掉在 0.1 以下。闸门若写
// `>= kDisplayStep`,这些**用户屏幕上读得到**的改动会被整批吞掉 —— 那是把摘要从
// 「太吵」改成「说谎」,比原缺陷更坏。半步长把两类分得干净:真差一档者最小
// 0.0999…(过闸),贴量化边界者差值趋近 0(不过闸),中间隔着一个数量级,
// 没有浮点误差跨得过去。
//
// **也没有把闸门定得比显示步长更大**(比如 pan 要求 ≥1.0):那会藏起屏幕上明明写着
// `12.0 → 12.5` 的段。摘要爆量的真正解法是 UI 侧的折叠(默认只出计数)——
// 判据这一侧只负责把**幅度不值一提的**滤掉,不负责减量。
//
// **闸门边界上那一档任意性是接受的,别为它加容差**([SL-274] 复审第 3 轮有建议给 `>=`
// 补 1e-5,理由是 `0.20f → 0.25f` 的 Δ 实为 0.049999997、会被滤掉)。半步长这个数守的是
// **成批**的一档改动(最小 0.0999…,余量一个数量级),它**不承诺** Δ 恰好等于 0.05 的
// 那一对落在哪边:0.0499999 与 0.0500001 对用户是同一件事,而现判据本来就明确接受
// 「屏幕上变了、幅度不值一提就不报」(`4.249 → 4.251` 那条用例正是这个意思)。
// 加容差只是把一条任意切线挪 0.02%,换来一个新魔数,且没有任何删之即红的通路。
//
// 非有限值由**本函数自己**那行 `isfinite` 接住,**不是**由 `sameAtDisplayPrecision`
// 接住:后者对非有限值返回 `false`(即判「不同」),控制流会**穿过**上面那个 early
// return 落到这里。删掉这一行,NaN 就会走进 `std::fabs` —— NaN 参与的比较恒假 ⇒
// 悄悄判成「没改」,与 `sameAtDisplayPrecision` 保守报改动的方向正好相反。用例钉着它。
inline bool changedAtDisplayPrecision(float from, float to) noexcept
{
    if (sameAtDisplayPrecision(from, to))
        return false;
    if (!std::isfinite(from) || !std::isfinite(to))
        return true; // 非有限:量化比较已判「不同」,幅度无从谈起 —— 保守报改动
    return std::fabs(static_cast<double>(to) - static_cast<double>(from)) >= kChangeAmplitudeGate;
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
// 两张表在生产侧都已按 t0 有序且互不重叠,但本函数**不要求调用方先排好序**:自建有序
// 下标(`std::sort` 对已有序输入近乎线性)。
//
// ⚠ 「与逐对枚举的贪心结果等价」这条**只在同一张表内的段互不重叠时成立**
// ([SL-255] 复审③;别把它当成「对任意输入都等价」)。理由:排序改变的正是**新段的
// 处理顺序**,而每条旧段只配一次 —— 两条新段争同一条旧段时,先处理的那条拿走它,
// 换个顺序就换个配法。段互不重叠时这种争抢不存在(重叠量为 0 的候选,在严格 `>` 的
// 选优判据下本就永远选不上),等价才成立。
// 生产侧这个前提由 `applyAnalysisSegments` 保证(它有 clash 检查 + 末尾 `std::sort`),
// 所以够不着;下面那条乱序用例也只打乱 `before` 的**输入顺序**,没有制造重叠。
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
        // [SL-274] 判据从「量化后不同」收紧成「量化后不同 **且** 幅度过半个显示步长」,
        // 理由逐字见 `changedAtDisplayPrecision` 的头注(贴着量化边界、只差 0.002 的一对
        // 值也曾进表,那正是用户实测「摘要弹出全轨全段」里幅度小到不值一提的那批)。
        if ((changedAtDisplayPrecision(o.pan, nn.pan) || changedAtDisplayPrecision(o.volDb, nn.volDb)) &&
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
