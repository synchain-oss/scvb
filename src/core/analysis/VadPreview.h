// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// VadPreview —— 拖动档预览的计算核([J146],契约 §1.18/§1.19「拖动档:每次调用即时重判决并回发
// VAD/边界预览」)。纯 C++17、JUCE-free、无 I/O、无全局状态,可离线单测。
//
// **与分析流水线是同一份实现,不是一份「差不多的」**:
//   · VAD:`runAnalysisPipeline` 调的 `runEnergyVad` = `computeVadBaseline` + `runEnergyVadOnBaseline`,
//     这里把前一步(与参数无关、最贵的那一半)缓存住,每次调用只跑后一步;
//   · 超长段谷切分:同一个 `splitLongVadSegments`,ℓ 包络也由同一个 `envelopeDbInto` 建。
// 所以「预览看到的 VAD 后验 / 每轨 S1 段」与「松手后那一趟流水线在同一份特征、同一组参数上
// 算出来的」逐位相同 —— core 用例 `[j146]` 直接对拍这一条。
//
// ⚠ 预览只到 **S1(每轨自己的 VAD 段 + 谷切分)**,不跑 S2 全局区间 / §5 指派 / §6 平衡:
// 那几步是跨轨的,松手后的段表会在**别的轨**的边界处再切开(同一区间 pan/vol 相同的相邻块
// 又会合回去),所以最终段数可以多于预览里的段数。预览回答的是「这组参数下每条轨哪里算有声、
// 长乐句在哪里切」,不是「最终段表长什么样」—— 后者要等松手那一趟跑完。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "analysis/EnergyVad.h"
#include "analysis/Segmentation.h"

namespace scvb::analysis
{

// 一条轨的缓存 —— 只含与参数无关的量(拖任何一根滑杆它都不变)。
struct VadPreviewTrackCache
{
    VadBaseline base; // ℓ(VAD 口径,2-hop 平滑)+ F / A / 活跃子集是否为空
    std::vector<float> envDb; // 谷切分口径的 ℓ(不平滑),长度同 base.l
    std::size_t hops() const noexcept { return base.l.size(); }
    std::size_t bytes() const noexcept { return (base.l.capacity() + envDb.capacity()) * sizeof(float); }
};

// 由一条轨的 kw(计算窗逐 hop,未覆盖 hop 填 0 —— 与流水线取样同口径)建缓存。
VadPreviewTrackCache buildVadPreviewTrackCache(const float* kwMs, std::size_t n);

// 在缓存上按参数跑 S1:VAD 状态机 + 后处理(runEnergyVadOnBaseline)→ 超长段谷切分
// (splitLongVadSegments)。返回 hop 域段(绝对时间线,按起点升序、互不重叠)。
// posteriorOut 可为 nullptr;非空时容量须 ≥ c.hops(),写截断后验 p[k] ∈ [0,1](§2.4,与流水线的
// `PipelineResult::vadPosterior` 同口径)。
std::vector<VadSegment> runVadPreviewTrack(VadPreviewTrackCache& c, std::int64_t firstHop, const VadParams& vad,
                                           const SegmentationParams& seg, double hopSec, float* posteriorOut);

// ---- [J146 复审] 两条调度判据(纯函数:processor 调、core 用例直接断言) ------------------------
//
// ① **缓存重建限频**。边播边采时特征每拍都在变,不限频的话每一次拖动调用都要重建一遍。判据:
//    这一拍需要重建(写回集里有轨的缓存缺失 / 旧了 / 计算窗变了)∧ 写回集里**至少一条轨**手上有
//    可沿用的缓存 ∧ 距上次重建不足 minMs。第二条只数**写回集里**的轨:上一拍唯一有缓存的轨这一拍
//    刚被关掉 / 断开、另一条刚进来时,若把前者也算进去就会判成「限频」,而新进来的那条没有缓存可沿用
//    ⇒ 这一拍一条轨都算不出 ⇒ 预览被收尾、下一拍再首建 —— 拖动中泳道闪一下。那种情形按首建处理。
inline bool vadPreviewRebuildThrottled(bool needRebuild, bool anyValidInWriteSet, std::int64_t nowMs,
                                       std::int64_t lastRebuildMs, std::int64_t minMs) noexcept
{
    return needRebuild && anyValidInWriteSet && nowMs - lastRebuildMs < minMs;
}

// 覆盖层的「修改序号」记账:限频期沿用的缓存算自**旧的计算窗**(分位数基准随窗变),即使这条轨自己
// 没被写过(别的轨在采、把时间线延长了),它的覆盖层也不是松手那一趟会写下的结果 ⇒ 记一个永远对不上的
// 哨兵,`waveformOf` 退回读 vadP(契约 §1.27「采集写入进行中」那条例外的实现)。不限频时照记缓存的序号。
inline constexpr std::uint64_t kVadPreviewOverlayStale = std::numeric_limits<std::uint64_t>::max();
inline std::uint64_t vadPreviewOverlaySeq(bool throttled, std::uint64_t cacheSeq) noexcept
{
    return throttled ? kVadPreviewOverlayStale : cacheSeq;
}

// ② **自适应占空比**(单次重判决耗时相对调用间隔太大时合并调用)。web 侧最多 ≤50Hz 连发、不等回执;
// 长会话(如 15 轨 × 60 分钟)里命中缓存的一次重判决就要 ~40ms,照单全收会把消息线程占满、调用在队列里
// 堆积(松手后的防抖与宿主存盘都被推迟)。判据:距上一次重判决**开始** ≥ max(上次耗时 × 2, floorMs)
// 才当场算,否则只记「有待算」,由 25Hz 定时器补算 —— 占空比 ≤ 50%,预览仍以最新参数为准。
// 典型会话(一次 ~3ms,调用间隔 25ms)下恒为「当场算」,行为与不合并时相同。从没算过 ⇒ 当场算。
inline bool vadPreviewComputeDue(double nowMs, double lastStartMs, double lastCostMs, double floorMs) noexcept
{
    if (!(lastStartMs > 0.0))
        return true;
    return nowMs - lastStartMs >= std::max(2.0 * lastCostMs, floorMs);
}

} // namespace scvb::analysis
