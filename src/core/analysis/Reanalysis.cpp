// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/Reanalysis.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace scvb::analysis
{

namespace
{

constexpr double kHopMs = 10.0; // 02 §0.2:hop = 10ms

int64_t msToSamples(double ms, int sampleRate)
{
    return static_cast<int64_t>(std::llround(ms / 1000.0 * static_cast<double>(sampleRate)));
}

int64_t hopToSamples(int64_t hop, int sampleRate)
{
    return hop * (sampleRate / 100); // 10ms/hop。
}

float dbAt(const SilenceWindow& win, int64_t hop)
{
    return win.loudnessDb[hop - win.firstHop];
}

// 在 [lo, hi) 内找「最靠近 hi」的静音 run(长度 ≥ minPauseHops),其右端 ≤ hi。
// 找到写 [outA, outB) 并返回 true;否则 false。
bool nearestSilenceLeft(const SilenceWindow& win, int64_t lo, int64_t hi, float exitDb, int64_t minPauseHops,
                        int64_t& outA, int64_t& outB)
{
    bool found = false;
    int64_t i = lo;
    while (i < hi)
    {
        if (dbAt(win, i) >= exitDb)
        {
            ++i;
            continue;
        }
        int64_t j = i;
        while (j < hi && dbAt(win, j) < exitDb)
            ++j;
        if (j - i >= minPauseHops)
        {
            outA = i;
            outB = j;
            found = true; // 继续向右找更近的。
        }
        i = j;
    }
    return found;
}

// 在 [lo, hi) 内找「最靠近 lo」的静音 run(长度 ≥ minPauseHops),其左端 ≥ lo。
bool nearestSilenceRight(const SilenceWindow& win, int64_t lo, int64_t hi, float exitDb, int64_t minPauseHops,
                         int64_t& outA, int64_t& outB)
{
    int64_t i = lo;
    while (i < hi)
    {
        if (dbAt(win, i) >= exitDb)
        {
            ++i;
            continue;
        }
        int64_t j = i;
        while (j < hi && dbAt(win, j) < exitDb)
            ++j;
        if (j - i >= minPauseHops)
        {
            outA = i;
            outB = j;
            return true; // 从左到右首个即最近。
        }
        i = j;
    }
    return false;
}

void putBytes(std::vector<uint8_t>& out, const void* p, std::size_t n)
{
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}

void putU32(std::vector<uint8_t>& out, uint32_t x)
{
    putBytes(out, &x, sizeof(x));
}
void putI64(std::vector<uint8_t>& out, int64_t x)
{
    putBytes(out, &x, sizeof(x));
}
void putU64(std::vector<uint8_t>& out, uint64_t x)
{
    putBytes(out, &x, sizeof(x));
}
void putF64(std::vector<uint8_t>& out, double x)
{
    putBytes(out, &x, sizeof(x));
}
void putU8(std::vector<uint8_t>& out, uint8_t x)
{
    putBytes(out, &x, sizeof(x));
}

} // namespace

void guardExtend(const SilenceWindow& win, int64_t r0, int64_t r1, const GuardParams& p, int64_t& outR0, int64_t& outR1)
{
    const int64_t minGuard = msToSamples(p.guardMs, p.sampleRate);
    outR0 = r0 - minGuard;
    outR1 = r1 + minGuard;

    if (win.loudnessDb == nullptr || win.numHops <= 0)
        return; // 无静音信息 → 固定 guardMs。

    const int64_t hs = hopToSamples(1, p.sampleRate);
    const int64_t minPauseHops = std::max<int64_t>(1, static_cast<int64_t>(std::llround(p.minPauseMs / kHopMs)));

    const int64_t h0 = r0 / hs;
    const int64_t h1 = r1 / hs;
    const int64_t winLo = win.firstHop;
    const int64_t winHi = win.firstHop + win.numHops;

    // 左:延伸到最近左静音 run 的右端(严格在 R 外,右端 ≤ h0)。
    int64_t a = 0;
    int64_t b = 0;
    const int64_t leftLo = std::max(winLo, int64_t{0});
    const int64_t leftHi = std::min(h0, winHi);
    if (leftLo < leftHi && nearestSilenceLeft(win, leftLo, leftHi, p.silenceExitDb, minPauseHops, a, b))
    {
        const int64_t candidate = b * hs;
        if (r0 - candidate > minGuard)
            outR0 = candidate;
    }

    // 右:延伸到最近右静音 run 的左端(严格在 R 外,左端 ≥ h1)。
    const int64_t rightLo = std::max(h1, winLo);
    const int64_t rightHi = winHi;
    if (rightLo < rightHi && nearestSilenceRight(win, rightLo, rightHi, p.silenceExitDb, minPauseHops, a, b))
    {
        const int64_t candidate = a * hs;
        if (candidate - r1 > minGuard)
            outR1 = candidate;
    }

    // 截断到窗口覆盖的样本范围。
    outR0 = std::max(outR0, winLo * hs);
    outR1 = std::min(outR1, winHi * hs);
}

std::vector<AnalysisSegment> spliceTrack(const std::vector<AnalysisSegment>& oldSegments,
                                         const std::vector<AnalysisSegment>& candidates,
                                         const std::vector<ExcludedRange>& excludedRanges, int64_t rPlus0,
                                         int64_t rPlus1, const MergeParams& p)
{
    // 候选在 R⁺ 边界截断(04 §4.3);零长/负长丢弃。
    std::vector<AnalysisSegment> clipped;
    clipped.reserve(candidates.size());
    for (const AnalysisSegment& c : candidates)
    {
        const int64_t t0 = std::max(c.t0Samples, rPlus0);
        const int64_t t1 = std::min(c.t1Samples, rPlus1);
        if (t1 <= t0)
            continue;
        AnalysisSegment cc = c;
        cc.t0Samples = t0;
        cc.t1Samples = t1;
        clipped.push_back(cc);
    }
    std::sort(clipped.begin(), clipped.end(),
              [](const AnalysisSegment& a, const AnalysisSegment& b) { return a.t0Samples < b.t0Samples; });
    return mergeTrackSegments(oldSegments, clipped, excludedRanges, rPlus0, rPlus1, p);
}

VersionAnalysis reanalyze(const VersionAnalysis& in, const ReanalysisRequest& req, const FeatureProvider& features,
                          const RecomputeFn& recompute, const GuardParams& guard, const MergeParams& merge)
{
    VersionAnalysis out = in; // 深拷贝全部轨 → T 外轨与 excluded 逐字节不变。

    for (const uint32_t track : req.tracks)
    {
        if (track >= out.numTracks())
            continue; // 越界轨忽略(绝不触碰其他数据)。

        int64_t rPlus0 = req.startSample;
        int64_t rPlus1 = req.endSample;
        SilenceWindow win; // 空窗口 → guardExtend 退化为固定 guardMs(两侧各 ≥1s)。
        if (features)
            win = features(track);
        guardExtend(win, req.startSample, req.endSample, guard, rPlus0, rPlus1);

        const std::vector<AnalysisSegment> candidates =
            recompute ? recompute(track, rPlus0, rPlus1) : std::vector<AnalysisSegment>{};

        out.segments[track] = spliceTrack(in.segments[track], candidates, in.excluded[track], rPlus0, rPlus1, merge);
    }
    return out;
}

std::vector<uint8_t> serializeVersion(const VersionAnalysis& v)
{
    std::vector<uint8_t> out;
    putU32(out, static_cast<uint32_t>(v.segments.size()));
    for (std::size_t t = 0; t < v.segments.size(); ++t)
    {
        const auto& segs = v.segments[t];
        putU32(out, static_cast<uint32_t>(segs.size()));
        for (const AnalysisSegment& s : segs)
        {
            putI64(out, s.t0Samples);
            putI64(out, s.t1Samples);
            putF64(out, s.pan);
            putF64(out, s.volDb);
            putU8(out, static_cast<uint8_t>(s.origin));
            putU8(out, s.locked ? 1u : 0u);
            putU64(out, s.captureEpoch);
            putU64(out, s.analysisEpoch);
            putF64(out, s.loudnessLufs);
            putF64(out, s.energyLinear);
        }
        const auto& ex = v.excluded[t];
        putU32(out, static_cast<uint32_t>(ex.size()));
        for (const ExcludedRange& e : ex)
        {
            putI64(out, e.t0);
            putI64(out, e.t1);
        }
    }
    return out;
}

} // namespace scvb::analysis
