// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// DistReadback —— 分布图「每轨当前值」的**读回口径**([SL-363])。
//
// 这份口径原本只长在 JS 里(`web/shared/readback.js` 的 `readbackSegsOf` / `curveSegmentAt` /
// `manualConstantOf`),给 Output 自己那张分布图用;Monitor 那张图的数是 native 侧 `VizPublisher`
// 独立算的(播放头精确时刻的 `CurveEvaluator` 求值)。两条链画的是**同一个量**,却各写一份 ——
// 用户 v5.6.8 实测「柱子位置明显不对,但一些片段是对齐的」就是这么来的:
//
//   · **段间空隙**:Output 保持**前一段**的值(显示层没有 ramp 模型,取那一刻的稳态值);
//     `CurveEvaluator` 在空隙里**过了 ramp 中点就切到后一段**。段只在已分析区域上产生,
//     而截图那个工程写着「已分析区域 2 段 · 合计 0:54」—— 时间线上大片没有段,播放头
//     落在空隙里的时刻远多于落在段内的时刻,这是分叉面积最大的一类。
//   · **过渡斜坡**:`CurveEvaluator` 在相邻段边界做 smoothstep 插值,窗口 `T_eff` 在 gap=0 那支
//     由限速反推、**最长 6 秒**(不是「80ms 一闪」);Output 在同一时刻显示的是段常值。
//   · 段**前**/段**后**两侧其实一致(都回填首段 / 保持末段值)—— 这一类不是病灶,别顺手「修」。
//
// 所以本文件把那条链搬到 C++ 来,`VizPublisher` 与 Output 前端从此读同一口径。**样本域**比较
// (JS 那侧是秒域:`samplesToSeconds` 对两边同除一个 sr,序关系不变,样本域还免掉一次浮点往返)。
//
// ⚠ 改这里必须同改 `web/shared/readback.js`,反之亦然 —— 跨语言没法共用一份代码,只能靠这条
// 注释与两侧的交叉引用把它们钉在一起(手法同 `FreezeBits.h` 的三侧口径)。判据面见
// `tests/core/test_viz_plane.cpp` 的 `[SL-363]` 用例与 `tests/host/test_host_harness.cpp` 的
// HOST SL-363(接线格)。

#include <cstdint>
#include <vector>

#include "../engine/FreezeBits.h"
#include "../state/StateCodec.h"

namespace scvb::output
{

// 「单段全时限 `UserEdited` 常值」= `setTrackManual` 手动接管通道的产物特征(契约 §1.16)。
// 命中返回该段,否则 nullptr。**判据逐字对齐 JS 的 `manualConstantOf`**:只看「段数 == 1」
// 与 origin,不看 t1 是不是那个 `1<<40` 哨兵 —— 哨兵在上桥时会被降级成「已知时间线末端」
// (`BridgeArgs.h` 的 `effectiveT1Samples`),JS 那侧根本看不到它,拿它当判据两侧必然分叉。
inline const scvb::state::Segment* manualConstantOf(const std::vector<scvb::state::Segment>& segs)
{
    if (segs.size() != 1)
    {
        return nullptr;
    }
    return scvb::state::segmentOrigin(segs.front().flags) == scvb::state::SegmentOrigin::UserEdited ? &segs.front()
                                                                                                    : nullptr;
}

// 曲线在某一时刻**所处的段**。空表返回 nullptr(调用方回落参数面)。
// 钳位口径逐条对齐 JS `curveSegmentAt`(它自己又对齐 `CurveEvaluator::valueAt` 的段选择):
//   · 首段之前 → 首段;末段之后 → 末段;段内 → 该段;段间空隙 → **前一段**。
// `t1 <= t0` 的**退化段**按开放尾段处理:`!(t1 > t0)` 那一支让「t ≥ t0」就算段内。
// ⚠ `setTrackManual` 的 `1<<40` 哨兵**不走**这一支 —— 它的 t1 远大于任何播放位置,
// `t < t1` 本来就成立,落在普通的「段内」那一支;「末段之后」那道闸同理够不着它。
// 两条路在这里同结果,但别把它们读成同一个分支。
inline const scvb::state::Segment* curveSegmentAt(const std::vector<scvb::state::Segment>& segs, std::int64_t t)
{
    if (segs.empty())
    {
        return nullptr;
    }
    const scvb::state::Segment& first = segs.front();
    const scvb::state::Segment& last = segs.back();
    if (t < first.t0)
    {
        return &first;
    }
    if (last.t1 > last.t0 && t >= last.t1)
    {
        return &last;
    }
    const scvb::state::Segment* hit = &first;
    for (const scvb::state::Segment& s : segs)
    {
        if (t >= s.t0 && (t < s.t1 || !(s.t1 > s.t0)))
        {
            return &s; // 段内(含开放尾段)
        }
        if (t >= s.t0)
        {
            hit = &s; // 已越过本段 ⇒ 暂记为「前一段」
        }
    }
    return hit;
}

// 一条轨的读回结果:pan / vol 两维各自该读的段;nullptr = 该维回落参数面。
struct DistReadback
{
    const scvb::state::Segment* pan = nullptr;
    const scvb::state::Segment* vol = nullptr;
    const scvb::state::Segment* manual = nullptr; // 命中的手动常值段(调用方要标「手动接管」时用)
};

// 优先级链(J78「显示的是该维度的权威」),逐条即 JS 的 `readbackSegsOf`:
//   · **冻结**维度 → 参数面(宿主自动化 / 冻结手动值当家,[J85]),不看段表;
//   · 有**手动常值段** → 该段,且**不看输出档**(手动接管写的是曲线真身,ON/OFF 听到的都是它);
//   · 否则输出 **ON** → 播放头所处的段;
//   · 否则输出 **OFF**(跟随宿主)→ 参数面;
//   · 段表为空 → 两维都 nullptr。
// `freezeBits` = `scvb::engine::freezeBitsOf(raw)` 的结果(唯一解码口径)。
inline DistReadback readbackSegsOf(const std::vector<scvb::state::Segment>& segs, int freezeBits, bool outputOn,
                                   std::int64_t t)
{
    DistReadback out;
    out.manual = manualConstantOf(segs);
    const scvb::state::Segment* seg =
        out.manual != nullptr ? out.manual : (outputOn ? curveSegmentAt(segs, t) : nullptr);
    out.pan = scvb::engine::freezeHasDim(freezeBits, /*isPan=*/true) ? nullptr : seg;
    out.vol = scvb::engine::freezeHasDim(freezeBits, /*isPan=*/false) ? nullptr : seg;
    return out;
}

} // namespace scvb::output
