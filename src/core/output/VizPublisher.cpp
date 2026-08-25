// SPDX-License-Identifier: GPL-3.0-or-later
#include "VizPublisher.h"

#include <algorithm>
#include <cmath>

namespace scvb::output
{

namespace
{
// ppq → 样本(需 tempo 有效)。bpm 非正即无效。
bool ppqToSamples(double ppq, double bpm, double sampleRate, std::int64_t& out)
{
    if (bpm <= 0.0 || sampleRate <= 0.0)
    {
        return false;
    }
    const double sec = ppq * 60.0 / bpm;
    if (!(sec >= 0.0) || sec > 24.0 * 3600.0)
    {
        return false;
    }
    out = static_cast<std::int64_t>(sec * sampleRate + 0.5);
    return true;
}
} // namespace

VizPublisher::VizPublisher(scvb::ISegmentBackend& backend, scvb::u32 group)
    : plane_(backend, group), snap_(std::make_unique<scvb::VizSnapshot>())
{
}

void VizPublisher::setGroup(scvb::u32 group)
{
    // 只换指向 + 丢弃增量基线;建段与否由调用方按 claim 态决定(见 OutputProcessor::syncVizSegment)。
    plane_.release();
    plane_.setGroupWriter(group);
    everBuiltLanes_ = false;
    everPublished_ = false;
    lastSpanSamples_ = 0;
}

scvb::InitResult VizPublisher::changeGroup(scvb::u32 group)
{
    // 换组 = 换段:上一组的车道内容对新组无意义,强制下一帧全量重发。
    everBuiltLanes_ = false;
    everPublished_ = false;
    lastSpanSamples_ = 0;
    return plane_.changeGroup(group);
}

void VizPublisher::release()
{
    plane_.release();
    everBuiltLanes_ = false;
    everPublished_ = false;
    lastSpanSamples_ = 0;
}

scvb::u64 VizPublisher::computeWindowSpan(const VizPublishInput& in) const
{
    if (in.sampleRate <= 0.0)
    {
        return 0;
    }

    std::int64_t maxEnd = 0;
    if (in.crvs != nullptr)
    {
        const std::size_t v = (in.versionActive >= 1 && in.versionActive <= scvb::state::kNumVersions)
                                  ? static_cast<std::size_t>(in.versionActive - 1)
                                  : 0;
        for (const auto& track : in.crvs->versions[v].tracks)
        {
            for (const auto& seg : track.segments)
            {
                // 「无末端」分段只以 t0 参与跨度(t1 是哨兵,不是真末端)。
                const std::int64_t end = seg.t1 >= kVizOpenEndedT1 ? seg.t0 : seg.t1;
                maxEnd = std::max(maxEnd, end);
            }
        }
    }
    if (in.playhead.timeSamples > 0)
    {
        maxEnd = std::max(maxEnd, in.playhead.timeSamples + 1);
    }

    const double quantumSamples = kWindowQuantumSec * in.sampleRate;
    const double minSamples = kMinWindowSec * in.sampleRate;
    double span = std::max(static_cast<double>(maxEnd), minSamples);
    // 向上取整到量化边界:播放推进时跨度保持稳定,车道无须逐帧重算。
    span = std::ceil(span / quantumSamples) * quantumSamples;
    span = std::min(span, kMaxWindowSec * in.sampleRate);
    return static_cast<scvb::u64>(span);
}

void VizPublisher::rebuildLanes(const VizPublishInput& in, scvb::u64 spanSamples)
{
    auto& s = *snap_;
    s.coveredMask = 0;
    s.clearLanes();
    for (scvb::u32 t = 0; t < scvb::kMaxChannels; ++t)
    {
        s.trackColor[t] = t + 1; // v1:调色板槽位恒 = 轨号(web --track-color-{n} 顺序即轨号)
        // 轨名随车道一起落段(图例需要;metaRevision 变化也会触发本函数)。
        s.label[t] = (t < scvb::state::kNumTracks) ? in.label[t] : std::string{};
    }
    if (spanSamples == 0 || in.sampleRate <= 0.0)
    {
        return;
    }

    const std::size_t vIdx = (in.versionActive >= 1 && in.versionActive <= scvb::state::kNumVersions)
                                 ? static_cast<std::size_t>(in.versionActive - 1)
                                 : 0;
    const double colSamples = static_cast<double>(spanSamples) / static_cast<double>(scvb::kVizColumns);
    const double windowStart = static_cast<double>(s.windowStartSamples);

    for (scvb::u32 t = 0; t < scvb::kMaxChannels && t < scvb::state::kNumTracks; ++t)
    {
        const bool hasSegments = in.crvs != nullptr && !in.crvs->versions[vIdx].tracks[t].segments.empty();
        const scvb::CurveEvaluator* curve = in.curves[t];
        if (!hasSegments || curve == nullptr)
        {
            continue; // 整轨保持哨兵 + 零覆盖(断线)
        }
        s.coveredMask |= (1u << t);

        // 覆盖位图:列区间与任一分段有交集即置位(保守口径,短分段不消失)。
        for (const auto& seg : in.crvs->versions[vIdx].tracks[t].segments)
        {
            const double segStart = static_cast<double>(seg.t0) - windowStart;
            const double segEnd = (seg.t1 >= kVizOpenEndedT1 ? static_cast<double>(spanSamples)
                                                             : static_cast<double>(seg.t1) - windowStart);
            if (segEnd <= 0.0 || segStart >= static_cast<double>(spanSamples) || segEnd <= segStart)
            {
                continue;
            }
            const double firstF = std::floor(std::max(0.0, segStart) / colSamples);
            // 半开区间 [segStart, segEnd):末列 = ceil(segEnd/col) - 1。
            const double lastF = std::ceil(std::min(static_cast<double>(spanSamples), segEnd) / colSamples) - 1.0;
            const auto first = static_cast<scvb::u32>(std::max(0.0, firstF));
            const auto last =
                static_cast<scvb::u32>(std::min(static_cast<double>(scvb::kVizColumns - 1), std::max(0.0, lastF)));
            for (scvb::u32 i = first; i <= last; ++i)
            {
                s.setCovered(t, i);
            }
        }

        // 车道:列中心时刻点采样(与引擎打印同源 —— 同一个 CurveEvaluator)。
        for (scvb::u32 i = 0; i < scvb::kVizColumns; ++i)
        {
            const double centerSamples = windowStart + (static_cast<double>(i) + 0.5) * colSamples;
            s.pan[t][i] = scvb::vizPackPan(curve->panAt(centerSamples / in.sampleRate));
        }
    }
}

bool VizPublisher::tick(scvb::u64 nowMs, const VizPublishInput& in)
{
    if (!plane_.isOpen())
    {
        return false;
    }
    if (everPublished_ && nowMs - lastPublishMs_ < kPublishIntervalMs)
    {
        return false;
    }

    auto& s = *snap_;
    const scvb::u64 span = computeWindowSpan(in);

    const bool needLanes = !everBuiltLanes_ || in.crvsRevision != lastCrvsRevision_ ||
                           in.metaRevision != lastMetaRevision_ || in.versionActive != lastVersionActive_ ||
                           span != lastSpanSamples_ || nowMs - lastLaneMs_ >= kLaneRefreshMaxMs;

    s.publishMs = nowMs;
    s.windowStartSamples = 0; // v1:窗口起点恒 = 工程起点
    s.windowSpanSamples = span;
    s.sampleRate = static_cast<scvb::u32>(in.sampleRate > 0.0 ? in.sampleRate + 0.5 : 0.0);
    s.versionActive = in.versionActive;
    s.onlineMask = in.enabledMask;
    s.stereoMask = in.stereoMask;
    s.leadMask = in.leadMask;

    // playhead / 循环区。
    const auto& p = in.playhead;
    s.playheadSamples = p.timeSamples;
    s.playheadEpoch = p.epoch;
    scvb::u32 flags = 0;
    if ((p.flags & scvb::engine::kPlayheadIsPlaying) != 0u)
    {
        flags |= scvb::kVizPlaying;
    }
    if ((p.flags & scvb::engine::kPlayheadIsLooping) != 0u)
    {
        flags |= scvb::kVizLooping;
    }
    s.loopStartSamples = -1;
    s.loopEndSamples = -1;
    if ((p.flags & scvb::engine::kPlayheadCycleValid) != 0u && (p.flags & scvb::engine::kPlayheadTempoValid) != 0u)
    {
        std::int64_t a = -1;
        std::int64_t b = -1;
        const double sr = p.sampleRate > 0.0 ? p.sampleRate : in.sampleRate;
        if (ppqToSamples(p.loopStartPpq, p.bpm, sr, a) && ppqToSamples(p.loopEndPpq, p.bpm, sr, b) && b > a)
        {
            s.loopStartSamples = a;
            s.loopEndSamples = b;
            flags |= scvb::kVizLoopValid;
        }
    }
    s.playheadFlags = flags;

    if (needLanes)
    {
        rebuildLanes(in, span);
        s.laneRevision += 1;
        lastLaneMs_ = nowMs;
        lastCrvsRevision_ = in.crvsRevision;
        lastMetaRevision_ = in.metaRevision;
        lastVersionActive_ = in.versionActive;
        lastSpanSamples_ = span;
        everBuiltLanes_ = true;
        ++laneRebuildCount_;
    }

    // 每轨当前值(分布图数据面)。**每帧都刷**,不受车道分频影响 —— 它们是「此刻」。
    // 位置在 rebuildLanes **之后**:clearLanes() 现在已不碰这三个值(名副其实了),
    // 但顺序上仍排后面当第二道保险 —— R2 就是「先算后清」栽的。
    // panNow/volDb 走播放头**精确时刻**求值(不是车道在播放头所在列的采样:那是列中心点采样,
    // 列宽 = span/1024,分布图要的是此刻);widthPct 直接来自参数。
    {
        const double headSec =
            (in.sampleRate > 0.0 && p.timeSamples >= 0) ? static_cast<double>(p.timeSamples) / in.sampleRate : 0.0;
        const std::size_t vIdx = (in.versionActive >= 1 && in.versionActive <= scvb::state::kNumVersions)
                                     ? static_cast<std::size_t>(in.versionActive - 1)
                                     : 0;
        for (scvb::u32 t = 0; t < scvb::kMaxChannels; ++t)
        {
            s.panNow[t] = scvb::kVizPanNone;
            s.volDb[t] = scvb::kVizPanNone;
            s.widthPct[t] = scvb::kVizPanNone;
            if (t >= scvb::state::kNumTracks)
            {
                continue;
            }
            const bool hasSegments = in.crvs != nullptr && !in.crvs->versions[vIdx].tracks[t].segments.empty();
            const scvb::CurveEvaluator* curve = in.curves[t];
            if (hasSegments && curve != nullptr)
            {
                s.panNow[t] = scvb::vizPackPan(curve->panAt(headSec));
                s.volDb[t] = scvb::vizPackFixed(curve->volAt(headSec), scvb::kVizVolDbMin, scvb::kVizVolDbMax);
            }
            const float w = in.widthPct[t];
            if (w == w) // 非 NaN
            {
                s.widthPct[t] = scvb::vizPackFixed(static_cast<double>(w), scvb::kVizWidthMin, scvb::kVizWidthMax);
            }
        }
    }

    plane_.publish(s, needLanes);
    lastPublishMs_ = nowMs;
    everPublished_ = true;
    ++publishCount_;
    return true;
}

} // namespace scvb::output
