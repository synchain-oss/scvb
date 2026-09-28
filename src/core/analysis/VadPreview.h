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

#include <cstddef>
#include <cstdint>
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

} // namespace scvb::analysis
