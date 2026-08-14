// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/Segmentation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace scvb::analysis
{

namespace
{

constexpr double kHopMs = 10.0; // 02 §0.2:hop = 10ms
constexpr double kEpsilon = 1e-9; // 谷底平坦区判等容差

// sensitivity → minDepth(dB),02 §3.2:minDepth = 6 · 2^((50−s)/50)。
double minDepthFromSensitivity(double s)
{
    return 6.0 * std::pow(2.0, (50.0 - s) / 50.0);
}

// movingAverage(ℓ, 5 hop):中心窗 [k−2, k+2],段内截断(02 §3.2 步骤 1)。
std::vector<double> smoothEnvelope(const float* l, int64_t startHop, int64_t endHop, int halfWindow)
{
    std::vector<double> lv(static_cast<std::size_t>(endHop - startHop));
    for (int64_t k = startHop; k < endHop; ++k)
    {
        double sum = 0.0;
        int cnt = 0;
        for (int64_t j = k - halfWindow; j <= k + halfWindow; ++j)
        {
            if (j >= startHop && j < endHop)
            {
                sum += static_cast<double>(l[j]);
                ++cnt;
            }
        }
        lv[static_cast<std::size_t>(k - startHop)] = sum / static_cast<double>(cnt);
    }
    return lv;
}

// 谷宽(半深全宽,ms):以 bottom+depth/2 为参考水位,数 ℓ_v 严格低于该水位的 hop 数 ×10ms。
double valleyWidthMs(const std::vector<double>& lv, int64_t startHop, int64_t kStar, double depthDb)
{
    const double bottom = lv[static_cast<std::size_t>(kStar - startHop)];
    const double ref = bottom + depthDb / 2.0;
    int64_t w0 = kStar;
    while (w0 > startHop && lv[static_cast<std::size_t>(w0 - 1 - startHop)] < ref)
        --w0;
    int64_t w1 = kStar;
    while (w1 + 1 < startHop + static_cast<int64_t>(lv.size()) && lv[static_cast<std::size_t>(w1 + 1 - startHop)] < ref)
        ++w1;
    return static_cast<double>(w1 - w0 + 1) * kHopMs;
}

// durationFit(02 §3.3):谷宽落在 [80,600]ms 记 1,线性衰减到 [40,1000]ms 外记 0。
double durationFit(double widthMs)
{
    if (widthMs >= 80.0 && widthMs <= 600.0)
        return 1.0;
    if (widthMs < 40.0 || widthMs > 1000.0)
        return 0.0;
    if (widthMs < 80.0)
        return (widthMs - 40.0) / 40.0; // 40..80 → 0..1
    return (1000.0 - widthMs) / 400.0; // 600..1000 → 1..0
}

// spindle(02 §3.3):谷底 ±30 hop 内包络与「中间高两端低」模板的归一化互相关(clamp 0..1)。
// v1 只用包络特征(ADR-007 特征流只有 kw_ms/peak),不用频带占比(02 §3.3)。方差 < 1e-12 → 0(0/0 守卫)。
double spindleScore(const std::vector<double>& lv, int64_t startHop, int64_t kStar)
{
    const int window = 30;
    int64_t lo = std::max(startHop, kStar - window);
    int64_t hi = std::min(startHop + static_cast<int64_t>(lv.size()) - 1, kStar + window);
    const int n = static_cast<int>(hi - lo + 1);
    if (n < 3)
        return 0.0;

    double meanE = 0.0;
    std::vector<double> e(static_cast<std::size_t>(n));
    std::vector<double> t(static_cast<std::size_t>(n));
    for (int j = 0; j < n; ++j)
    {
        const int64_t hop = lo + j;
        e[static_cast<std::size_t>(j)] = lv[static_cast<std::size_t>(hop - startHop)];
        // 「中间高两端低」模板:中心 1、两端 0 的对称三角。
        t[static_cast<std::size_t>(j)] =
            1.0 - static_cast<double>(std::llabs(hop - kStar)) / static_cast<double>(window);
        meanE += e[static_cast<std::size_t>(j)];
    }
    meanE /= static_cast<double>(n);

    double varE = 0.0;
    for (const double x : e)
    {
        const double d = x - meanE;
        varE += d * d;
    }
    if (varE < 1e-12)
        return 0.0;

    // 模板均值与方差(对称三角非零方差,无须守卫)。
    double meanT = 0.0;
    for (const double x : t)
        meanT += x;
    meanT /= static_cast<double>(n);
    double varT = 0.0;
    for (const double x : t)
    {
        const double d = x - meanT;
        varT += d * d;
    }

    double cov = 0.0;
    for (int j = 0; j < n; ++j)
        cov += (e[static_cast<std::size_t>(j)] - meanE) * (t[static_cast<std::size_t>(j)] - meanT);

    const double denom = std::sqrt(varE * varT);
    if (denom < 1e-12)
        return 0.0;
    const double corr = cov / denom;
    return std::max(0.0, std::min(1.0, corr));
}

bool byT0(const AnalysisSegment& a, const AnalysisSegment& b)
{
    return a.t0Samples < b.t0Samples;
}

int64_t msToSamples(double ms, int sampleRate)
{
    return static_cast<int64_t>(std::llround(ms / 1000.0 * static_cast<double>(sampleRate)));
}

int64_t overlapLength(int64_t a0, int64_t a1, int64_t b0, int64_t b1)
{
    return std::max<int64_t>(0, std::min(a1, b1) - std::max(a0, b0));
}

// 重叠是否严格 > 50%(以候选段自身长度计,02 §3.5 步骤 b/c 同口径)。
bool overlapExceedsHalf(int64_t segLen, int64_t overlap)
{
    return segLen > 0 && overlap * 2 > segLen;
}

// 邻接同值段 merge 的「同值」判定(曲线值 = pan/vol;origin/locked 属于曲线语义一并比较)。
bool sameCurveValues(const AnalysisSegment& a, const AnalysisSegment& b)
{
    return a.pan == b.pan && a.volDb == b.volDb && a.origin == b.origin && a.locked == b.locked;
}

// 活跃集合对称差大小(A/B 升序)。
int symmetricDifferenceSize(const std::vector<int>& a, const std::vector<int>& b)
{
    std::size_t i = 0;
    std::size_t j = 0;
    int diff = 0;
    while (i < a.size() || j < b.size())
    {
        if (i < a.size() && (j >= b.size() || a[i] < b[j]))
        {
            ++diff;
            ++i;
        }
        else if (j < b.size() && (i >= a.size() || b[j] < a[i]))
        {
            ++diff;
            ++j;
        }
        else
        {
            ++i;
            ++j;
        }
    }
    return diff;
}

} // namespace

double SegmentationParams::minDepthDb() const
{
    return minDepthFromSensitivity(sensitivity);
}

std::vector<Valley> detectValleys(const float* l, int64_t startHop, int64_t endHop, const SegmentationParams& p)
{
    std::vector<Valley> valleys;
    if (l == nullptr || endHop - startHop < 2)
        return valleys;

    const std::vector<double> lv = smoothEnvelope(l, startHop, endHop, p.smoothHops / 2);
    const int64_t n = endHop - startHop;

    // 谷底平坦区:极大连续等值区间,且严格低于左右邻居(02 §3.2 步骤 2a「局部极小」)。
    struct Run
    {
        int64_t a; // 绝对 hop,闭区间 [a,b]
        int64_t b;
        // 谷底代表 = 平坦区中心(四舍五入),使对称谷的切分点落在名义位置。
        int64_t rep() const
        {
            return static_cast<int64_t>(std::llround((static_cast<double>(a) + static_cast<double>(b)) / 2.0));
        }
    };
    std::vector<Run> runs;
    int64_t i = 0;
    while (i < n)
    {
        int64_t a = i;
        while (i + 1 < n && std::abs(lv[static_cast<std::size_t>(i + 1)] - lv[static_cast<std::size_t>(a)]) < kEpsilon)
            ++i;
        const int64_t b = i;
        const bool leftHigher =
            (a > 0) && (lv[static_cast<std::size_t>(a - 1)] > lv[static_cast<std::size_t>(a)] + kEpsilon);
        const bool rightHigher =
            (b < n - 1) && (lv[static_cast<std::size_t>(b + 1)] > lv[static_cast<std::size_t>(b)] + kEpsilon);
        // 局部极小:内部平坦区需两侧严格更高;贴段端平坦区需另一侧严格更高;
        // 全段同值(平坦)→ 非谷。
        bool isValley = false;
        if (a > 0 && b < n - 1)
            isValley = leftHigher && rightHigher;
        else if (a == 0 && b == n - 1)
            isValley = false;
        else
            isValley = leftHigher || rightHigher;
        if (isValley)
            runs.push_back({startHop + a, startHop + b});
        i = b + 1;
    }

    for (std::size_t ri = 0; ri < runs.size(); ++ri)
    {
        const int64_t kStar = runs[ri].rep();
        const int64_t leftBound = (ri == 0) ? startHop : runs[ri - 1].rep();
        const int64_t rightBound = (ri + 1 == runs.size()) ? (endHop - 1) : runs[ri + 1].rep();

        // 左/右侧峰 = [leftBound, k*] 与 [k*, rightBound] 的 max ℓ_v(侧峰边界固定,无自指)。
        double leftPeak = lv[static_cast<std::size_t>(leftBound - startHop)];
        for (int64_t k = leftBound; k <= kStar; ++k)
            leftPeak = std::max(leftPeak, lv[static_cast<std::size_t>(k - startHop)]);
        double rightPeak = lv[static_cast<std::size_t>(kStar - startHop)];
        for (int64_t k = kStar; k <= rightBound; ++k)
            rightPeak = std::max(rightPeak, lv[static_cast<std::size_t>(k - startHop)]);

        const double bottom = lv[static_cast<std::size_t>(kStar - startHop)];
        const double depth = std::min(leftPeak, rightPeak) - bottom;
        const double width = valleyWidthMs(lv, startHop, kStar, depth);
        const double breath = 0.5 * spindleScore(lv, startHop, kStar) + 0.5 * durationFit(width);
        const double score = p.w1 * depth + p.w2 * (width / 100.0) + p.w3 * breath;

        Valley v;
        v.hop = kStar;
        v.depthDb = depth;
        v.widthMs = width;
        v.breath = breath;
        v.score = score;
        valleys.push_back(v);
    }
    return valleys;
}

ValleySplitResult splitValleys(const float* l, int64_t startHop, int64_t endHop, const SegmentationParams& p)
{
    ValleySplitResult result;
    if (l == nullptr || endHop <= startHop)
        return result;

    const int64_t maxHops = static_cast<int64_t>(std::llround(p.maxSegmentS * 1000.0 / kHopMs));
    const int64_t minHops = static_cast<int64_t>(std::llround(p.minSegmentMs / kHopMs));

    // 递归二分(02 §3.2 步骤 4)。使用显式栈避免深递归。
    struct Task
    {
        int64_t t0;
        int64_t t1;
    };
    std::vector<Task> stack;
    stack.push_back({startHop, endHop});

    while (!stack.empty())
    {
        const Task seg = stack.back();
        stack.pop_back();

        if (seg.t1 - seg.t0 <= maxHops)
        {
            result.segments.push_back({seg.t0, seg.t1});
            continue;
        }

        const auto allValleys = detectValleys(l, seg.t0, seg.t1, p);
        const double minDepth = p.minDepthDb();

        // 段内候选谷:depth > minDepth 且距两端 ≥ minSegment(02 §3.2 步骤 3/4)。
        const Valley* best = nullptr;
        for (const Valley& v : allValleys)
        {
            if (v.depthDb <= minDepth)
                continue;
            if (v.hop - seg.t0 < minHops || seg.t1 - v.hop < minHops)
                continue;
            if (best == nullptr)
            {
                best = &v;
                continue;
            }
            // argmax score;平局 → 距段中点近者;再平局 → 较早者。
            if (v.score > best->score)
            {
                best = &v;
            }
            else if (v.score == best->score)
            {
                const int64_t mid = (seg.t0 + seg.t1) / 2;
                const int64_t dBest = std::llabs(best->hop - mid);
                const int64_t dV = std::llabs(v.hop - mid);
                if (dV < dBest)
                    best = &v;
                else if (dV == dBest && v.hop < best->hop)
                    best = &v;
            }
        }

        if (best == nullptr)
        {
            result.noNaturalCut = true; // 未找到自然切点(02 §3.2 步骤 4,UI 提示用户可手切)
            result.segments.push_back({seg.t0, seg.t1});
            continue;
        }

        stack.push_back({best->hop, seg.t1});
        stack.push_back({seg.t0, best->hop});
    }

    std::sort(result.segments.begin(), result.segments.end(),
              [](const VadSegment& a, const VadSegment& b) { return a.startHop < b.startHop; });
    return result;
}

std::vector<GlobalInterval> buildGlobalIntervals(const std::vector<std::vector<AnalysisSegment>>& trackSegments,
                                                 int sampleRate, double minIntervalMs)
{
    std::vector<GlobalInterval> result;
    const int numTracks = static_cast<int>(trackSegments.size());
    if (numTracks == 0)
        return result;

    // 1. 收集全部边界 → 排序去重。
    std::vector<int64_t> bounds;
    for (const auto& segs : trackSegments)
    {
        for (const auto& s : segs)
        {
            bounds.push_back(s.t0Samples);
            bounds.push_back(s.t1Samples);
        }
    }
    if (bounds.empty())
        return result;
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    // 2. 切区间并算活跃集合 A_t。
    struct Interval
    {
        int64_t t0;
        int64_t t1;
        std::vector<int> tracks;
    };
    std::vector<Interval> intervals;
    for (std::size_t b = 0; b + 1 < bounds.size(); ++b)
    {
        const int64_t t0 = bounds[b];
        const int64_t t1 = bounds[b + 1];
        if (t1 <= t0)
            continue;
        Interval iv;
        iv.t0 = t0;
        iv.t1 = t1;
        for (int t = 0; t < numTracks; ++t)
        {
            for (const auto& s : trackSegments[static_cast<std::size_t>(t)])
            {
                if (s.t0Samples <= t0 && t1 <= s.t1Samples)
                {
                    iv.tracks.push_back(t);
                    break;
                }
            }
        }
        intervals.push_back(std::move(iv));
    }

    // 3. 碎片合并:区间长 < minGlobalInterval → 并入对称差更小的相邻区间;平局并入前侧。
    const int64_t minLen = msToSamples(minIntervalMs, sampleRate);
    bool merged = true;
    while (merged && intervals.size() > 1)
    {
        merged = false;
        for (std::size_t idx = 0; idx < intervals.size(); ++idx)
        {
            if (intervals[idx].t1 - intervals[idx].t0 >= minLen)
                continue;
            if (idx == 0)
            {
                // 仅右侧邻居:并入前侧(右侧)。
                intervals[idx + 1].t0 = intervals[idx].t0;
                intervals.erase(intervals.begin() + static_cast<std::ptrdiff_t>(idx));
            }
            else if (idx + 1 == intervals.size())
            {
                intervals[idx - 1].t1 = intervals[idx].t1;
                intervals.erase(intervals.begin() + static_cast<std::ptrdiff_t>(idx));
            }
            else
            {
                const int dl = symmetricDifferenceSize(intervals[idx].tracks, intervals[idx - 1].tracks);
                const int dr = symmetricDifferenceSize(intervals[idx].tracks, intervals[idx + 1].tracks);
                if (dl <= dr) // 平局 → 前侧(左)。
                {
                    intervals[idx - 1].t1 = intervals[idx].t1;
                }
                else
                {
                    intervals[idx + 1].t0 = intervals[idx].t0;
                }
                intervals.erase(intervals.begin() + static_cast<std::ptrdiff_t>(idx));
            }
            merged = true;
            break; // 结构已变,重扫。
        }
    }

    // 4. 相邻同活跃集合合并。
    std::vector<Interval> collapsed;
    for (const Interval& iv : intervals)
    {
        if (!collapsed.empty() && collapsed.back().tracks == iv.tracks && collapsed.back().t1 == iv.t0)
        {
            collapsed.back().t1 = iv.t1;
        }
        else
        {
            collapsed.push_back(iv);
        }
    }

    result.reserve(collapsed.size());
    for (const Interval& iv : collapsed)
    {
        GlobalInterval g;
        g.t0 = iv.t0;
        g.t1 = iv.t1;
        g.tracks = iv.tracks;
        result.push_back(std::move(g));
    }
    return result;
}

void expandRegion(int64_t r0, int64_t r1, double guardMs, int sampleRate, int64_t& outR0, int64_t& outR1)
{
    const int64_t guard = msToSamples(guardMs, sampleRate);
    outR0 = r0 - guard;
    outR1 = r1 + guard;
}

std::vector<AnalysisSegment> mergeTrackSegments(const std::vector<AnalysisSegment>& oldSegments,
                                                const std::vector<AnalysisSegment>& newCandidates,
                                                const std::vector<ExcludedRange>& excludedRanges, int64_t rPlus0,
                                                int64_t rPlus1, const MergeParams& p)
{
    std::vector<AnalysisSegment> result; // R⁺ 外侧半段(原样保留/跨界分裂)
    std::vector<AnalysisSegment> keep; // S_keep 占位段(02 §3.5 步骤 2a)
    std::vector<AnalysisSegment> carved; // 幸存候选(挖洞后,02 §3.5 步骤 2c)

    // 2a. 旧段分桶:R⁺ 外原样保留;跨界段在边界分裂,外侧半段继承原 pan/vol(04 §4.3);
    //     内侧 user 段进占位,内侧 auto 段丢弃(被候选取代)。
    for (const AnalysisSegment& s : oldSegments)
    {
        if (s.t1Samples <= rPlus0 || s.t0Samples >= rPlus1)
        {
            result.push_back(s); // 完全在 R⁺ 外,一个字节不动。
            continue;
        }
        if (s.t0Samples < rPlus0)
        {
            AnalysisSegment outside = s;
            outside.t1Samples = rPlus0;
            result.push_back(outside); // 左侧跨界:外侧半段继承原值。
        }
        if (s.t1Samples > rPlus1)
        {
            AnalysisSegment outside = s;
            outside.t0Samples = rPlus1;
            result.push_back(outside); // 右侧跨界:外侧半段继承原值。
        }
        AnalysisSegment inside = s;
        inside.t0Samples = std::max(s.t0Samples, rPlus0);
        inside.t1Samples = std::min(s.t1Samples, rPlus1);
        if (inside.length() <= 0)
            continue;
        if (inside.isUserSegment())
            keep.push_back(inside); // 占位[J34]:origin != Auto || locked。
        // auto 内侧段丢弃。
    }
    std::sort(keep.begin(), keep.end(), byT0);

    // 2b. 候选与任一 ExcludedRange 重叠 >50%(以候选自身长度计)→ 丢弃(防复活,SEG-6)。
    std::vector<AnalysisSegment> surviving;
    surviving.reserve(newCandidates.size());
    for (const AnalysisSegment& c : newCandidates)
    {
        const int64_t len = c.length();
        if (len <= 0)
            continue;
        bool resurrected = false;
        for (const ExcludedRange& e : excludedRanges)
        {
            if (overlapExceedsHalf(len, overlapLength(c.t0Samples, c.t1Samples, e.t0, e.t1)))
            {
                resurrected = true;
                break;
            }
        }
        if (!resurrected)
            surviving.push_back(c);
    }

    // 2c. 候选与占位段重叠 >50%(总和,候选自身长度计)→ 丢弃;≤50% → 挖洞,挖洞后短于
    //     minSegment 丢弃(02 §3.5 步骤 c;维持「按 t0 排序互不重叠」不变量)。
    const int64_t minLen = msToSamples(p.minSegmentMs, p.sampleRate);
    for (const AnalysisSegment& c : surviving)
    {
        const int64_t len = c.length();
        int64_t totalOverlap = 0;
        for (const AnalysisSegment& k : keep)
            totalOverlap += overlapLength(c.t0Samples, c.t1Samples, k.t0Samples, k.t1Samples);
        if (overlapExceedsHalf(len, totalOverlap))
            continue;

        std::vector<AnalysisSegment> pieces;
        pieces.push_back(c);
        for (const AnalysisSegment& k : keep)
        {
            std::vector<AnalysisSegment> next;
            for (const AnalysisSegment& piece : pieces)
            {
                if (overlapLength(piece.t0Samples, piece.t1Samples, k.t0Samples, k.t1Samples) == 0)
                {
                    next.push_back(piece);
                    continue;
                }
                if (piece.t0Samples < k.t0Samples)
                {
                    AnalysisSegment left = piece;
                    left.t1Samples = k.t0Samples;
                    next.push_back(left);
                }
                if (k.t1Samples < piece.t1Samples)
                {
                    AnalysisSegment right = piece;
                    right.t0Samples = k.t1Samples;
                    next.push_back(right);
                }
            }
            pieces = std::move(next);
            if (pieces.empty())
                break;
        }
        for (const AnalysisSegment& piece : pieces)
        {
            if (piece.length() >= minLen)
                carved.push_back(piece);
        }
    }

    // 2d. 幸存候选边界落在 locked 段边界 ±50ms 内 → 吸附到锁定边界(只作用于候选,不触碰
    //     R⁺ 外侧半段与占位段 —— 否则会破坏不变式 1/2)。
    std::vector<int64_t> lockedBounds;
    for (const AnalysisSegment& s : oldSegments)
    {
        if (s.locked)
        {
            lockedBounds.push_back(s.t0Samples);
            lockedBounds.push_back(s.t1Samples);
        }
    }
    const int64_t snap = msToSamples(p.snapMs, p.sampleRate);
    for (AnalysisSegment& c : carved)
    {
        for (const int64_t b : lockedBounds)
        {
            if (std::llabs(c.t0Samples - b) <= snap)
                c.t0Samples = b;
            if (std::llabs(c.t1Samples - b) <= snap)
                c.t1Samples = b;
        }
    }

    // 2e. 合并三类(R⁺ 外侧半段 ∪ 占位段 ∪ 幸存候选)→ 排序 + 邻接同值 merge + 丢弃零长,
    //     维持「按 t0 排序互不重叠」不变量。
    std::vector<AnalysisSegment> combined;
    combined.reserve(result.size() + keep.size() + carved.size());
    for (const AnalysisSegment& s : result)
        combined.push_back(s);
    for (const AnalysisSegment& k : keep)
        combined.push_back(k);
    for (const AnalysisSegment& c : carved)
        combined.push_back(c);

    std::sort(combined.begin(), combined.end(), byT0);
    std::vector<AnalysisSegment> out;
    out.reserve(combined.size());
    for (const AnalysisSegment& s : combined)
    {
        if (s.length() <= 0)
            continue;
        if (!out.empty() && out.back().t1Samples == s.t0Samples && sameCurveValues(out.back(), s))
        {
            out.back().t1Samples = s.t1Samples;
        }
        else
        {
            out.push_back(s);
        }
    }
    return out;
}

} // namespace scvb::analysis
