// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/AnalysisPipeline.h"

#include <algorithm>
#include <cmath>

namespace scvb::analysis
{

namespace
{

bool cancelled(const PipelineCancelFn& fn)
{
    return fn && fn();
}

void report(const PipelineProgressFn& fn, float p)
{
    if (fn)
    {
        fn(p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p));
    }
}

// 段内平均 K 加权能量 → 平衡用的线性能量 z(§4 的总能量口径)。
double meanKw(const std::vector<float>& kw, std::int64_t begin, std::int64_t end)
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

// K 加权均方 → LUFS(ITU-R BS.1770 的 −0.691 偏置)。静音回 −inf 的替身 −120。
double lufsFromMeanKw(double m)
{
    return m > 0.0 ? (10.0 * std::log10(m) - 0.691) : -120.0;
}

void addWarningOnce(std::vector<std::string>& out, const std::string& w)
{
    if (w.empty())
    {
        return;
    }
    if (std::find(out.begin(), out.end(), w) == out.end())
    {
        out.push_back(w);
    }
}

} // namespace

PipelineResult runAnalysisPipeline(const std::array<PipelineTrackFeatures, kPipelineTracks>& features,
                                   const PipelineConfig& cfg, const PipelineProgressFn& onProgress,
                                   const PipelineCancelFn& shouldCancel)
{
    PipelineResult result;

    const double sr = cfg.sampleRate > 0.0 ? cfg.sampleRate : 48000.0;
    const int hopMs = cfg.hopMs > 0 ? cfg.hopMs : 10;
    const double hopSec = static_cast<double>(hopMs) / 1000.0;
    const std::int64_t hopSamples = static_cast<std::int64_t>(std::llround(hopSec * sr));
    if (hopSamples <= 0 || cfg.rangeEndSample <= cfg.rangeStartSample)
    {
        return result;
    }

    const std::int64_t firstHop = cfg.rangeStartSample / hopSamples;

    // ---- S1:逐轨 VAD → 超长段谷切分 → 样本域段 --------------------------------------
    std::vector<std::vector<AnalysisSegment>> trackSegments(static_cast<std::size_t>(kPipelineTracks));

    for (int t = 0; t < kPipelineTracks; ++t)
    {
        report(onProgress, 0.05f + 0.45f * (static_cast<float>(t) / static_cast<float>(kPipelineTracks)));
        if (cancelled(shouldCancel))
        {
            result.cancelled = true;
            return result;
        }

        const auto& f = features[static_cast<std::size_t>(t)];
        const auto& tc = cfg.tracks[static_cast<std::size_t>(t)];
        // 关掉的轨、没有采集数据的轨:不产生段(§1 布防口径 {enabled 轨})。
        if (!tc.enabled || !f.anyCovered || f.kwMs.empty())
        {
            continue;
        }

        const VadResult vad = runEnergyVad(f.kwMs.data(), f.kwMs.size(), firstHop, cfg.vad, nullptr);
        addWarningOnce(result.warnings, vad.warningMessage() != nullptr ? vad.warningMessage() : std::string{});

        // 超长段按谷切分(§3.2):只对超过 maxSegment 的段做,短段原样保留。
        const std::int64_t maxHops = static_cast<std::int64_t>(std::llround(cfg.segmentation.maxSegmentS / hopSec));
        std::vector<VadSegment> hopSegs;
        for (const auto& vs : vad.segments)
        {
            if (maxHops > 0 && (vs.endHop - vs.startHop) > maxHops)
            {
                const ValleySplitResult split =
                    splitValleys(f.kwMs.data(), vs.startHop - firstHop, vs.endHop - firstHop, cfg.segmentation);
                if (!split.segments.empty())
                {
                    for (const auto& sub : split.segments)
                    {
                        hopSegs.push_back(VadSegment{sub.startHop + firstHop, sub.endHop + firstHop});
                    }
                    if (split.noNaturalCut)
                    {
                        addWarningOnce(result.warnings, "segmentation.noNaturalCut");
                    }
                    continue;
                }
            }
            hopSegs.push_back(vs);
        }

        auto& out = trackSegments[static_cast<std::size_t>(t)];
        out.reserve(hopSegs.size());
        for (const auto& hs : hopSegs)
        {
            if (hs.endHop <= hs.startHop)
            {
                continue;
            }
            AnalysisSegment as;
            as.t0Samples = hs.startHop * hopSamples;
            as.t1Samples = hs.endHop * hopSamples;
            as.origin = Origin::Auto;
            const double m = meanKw(f.kwMs, hs.startHop - firstHop, hs.endHop - firstHop);
            as.energyLinear = m;
            as.loudnessLufs = lufsFromMeanKw(m);
            out.push_back(as);
        }
        if (!out.empty())
        {
            ++result.tracksTouched;
        }
    }

    report(onProgress, 0.55f);
    if (cancelled(shouldCancel))
    {
        result.cancelled = true;
        return result;
    }

    // ---- S2:全局区间(§3.4)-----------------------------------------------------------
    const std::vector<GlobalInterval> intervals =
        buildGlobalIntervals(trackSegments, static_cast<int>(std::llround(sr)));
    result.intervals = static_cast<int>(intervals.size());
    if (intervals.empty())
    {
        return result;
    }

    // ---- S3/S4:逐区间指派 + 平衡,回写每轨段 ------------------------------------------
    // 上一区间的解用于连续性项(w_cont):按轨记住 pan。
    std::array<double, kPipelineTracks> prevPan{};
    std::array<bool, kPipelineTracks> hasPrev{};
    for (int t = 0; t < kPipelineTracks; ++t)
    {
        prevPan[static_cast<std::size_t>(t)] = cfg.tracks[static_cast<std::size_t>(t)].currentPan;
    }

    std::array<std::vector<AnalysisSegment>, kPipelineTracks> assigned;

    for (std::size_t i = 0; i < intervals.size(); ++i)
    {
        report(onProgress, 0.55f + 0.4f * (static_cast<float>(i) / static_cast<float>(intervals.size())));
        if (cancelled(shouldCancel))
        {
            result.cancelled = true;
            return result;
        }

        const GlobalInterval& gi = intervals[i];
        if (gi.tracks.empty() || gi.t1 <= gi.t0)
        {
            continue;
        }

        // 本区间的活跃轨 → TrackMeta(z 取该区间内的平均能量)。
        std::vector<TrackMeta> metas;
        metas.reserve(gi.tracks.size());
        for (const int t : gi.tracks)
        {
            if (t < 0 || t >= kPipelineTracks)
            {
                continue;
            }
            const auto& tc = cfg.tracks[static_cast<std::size_t>(t)];
            const auto& f = features[static_cast<std::size_t>(t)];

            TrackMeta m;
            m.priority = tc.priority;
            m.pairId = tc.pairId;
            m.leadLock = tc.leadLock;
            m.leadVolExempt = tc.leadVolExempt;
            m.freeze = tc.freeze;
            m.participateInAutoPan = tc.participateInAutoPan;
            m.source = tc.source;
            m.currentPan = tc.currentPan;
            m.hasPrev = hasPrev[static_cast<std::size_t>(t)];
            m.prevPan = prevPan[static_cast<std::size_t>(t)];

            const std::int64_t b = gi.t0 / hopSamples - firstHop;
            const std::int64_t e = gi.t1 / hopSamples - firstHop;
            const double z = meanKw(f.kwMs, b, e);
            m.z = z;
            if (tc.source == SourceChannels::Stereo)
            {
                m.zL = z * 0.5; // 未分通道测量 → 等分(AutoAssign 的默认口径)
                m.zR = z * 0.5;
            }
            else
            {
                m.zL = z;
                m.zR = 0.0;
            }
            metas.push_back(m);
        }
        if (metas.empty())
        {
            continue;
        }

        const BalanceResult br = solveBalanceWithFallback(metas, cfg.assign, cfg.balance);
        for (const auto& w : br.warnings)
        {
            addWarningOnce(result.warnings, w);
        }

        // 回写:区间 × 活跃轨 → 一段常值(pan 来自指派,volDb 来自平衡修正 u)。
        std::size_t k = 0;
        for (const int t : gi.tracks)
        {
            if (t < 0 || t >= kPipelineTracks)
            {
                continue;
            }
            const double pan = k < br.pans.size() ? br.pans[k] : 0.0;
            const double u = k < br.u.size() ? br.u[k] : 0.0;
            ++k;

            AnalysisSegment as;
            as.t0Samples = gi.t0;
            as.t1Samples = gi.t1;
            as.pan = pan;
            as.volDb = u;
            as.origin = Origin::Auto;
            assigned[static_cast<std::size_t>(t)].push_back(as);

            prevPan[static_cast<std::size_t>(t)] = pan;
            hasPrev[static_cast<std::size_t>(t)] = true;
        }
    }

    // 相邻同值段合并(纯瘦身:段数直接影响 CRVS 体积与 UI 段表长度)。
    for (int t = 0; t < kPipelineTracks; ++t)
    {
        const auto& src = assigned[static_cast<std::size_t>(t)];
        auto& dst = result.segments[static_cast<std::size_t>(t)];
        for (const auto& s : src)
        {
            if (!dst.empty() && dst.back().t1Samples == s.t0Samples && std::abs(dst.back().pan - s.pan) < 1e-6 &&
                std::abs(dst.back().volDb - s.volDb) < 1e-6)
            {
                dst.back().t1Samples = s.t1Samples;
                continue;
            }
            dst.push_back(s);
        }
    }

    report(onProgress, 1.0f);
    return result;
}

} // namespace scvb::analysis
